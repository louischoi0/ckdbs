# Work order DT - a write that gives its core back: the write path's page miss off the reactor, and a write walk that yields

Status: **CLOSED NARROW 2026-11-26** at `6e43ec8c0`
(`v3.4.17-154-g6e43ec8c0`), the walk's yield and its cancel built, the
write's page miss not (§7, "DT-S4: the close"): closed on CLA's reading of
the close measurement, adopted on the standing go-ahead and not the
operator's mark, and landed on `main` under the push the operator
authorized ahead of the close (`raft-marks-2026-11-26.md` §4). Opened
2026-11-26 on worktree `dt-write-path-yield` (branch
`worktree-dt-write-path-yield`) from `60dc593af`, under the operator's
standing go-ahead of 2026-11-26 (`raft-marks-2026-11-26.md` §4): every
DT-Q item is **adopted on that go-ahead, never marked by the operator item
by item** (§4, "Marks"). **DT-S0 read DT-Q1's bar per mechanism** (§7):
the cold-write stall is under the bar, so §2.1 and §2.2 - the run-again
and the park at a cold leaf - are **not built**, and DT-S1 has no subject;
the long-walk stall is far over it, so §2.3 and §2.4 are built. What
follows is the order as written, with §7 saying what each stage did.

As written: **WRITTEN 2026-11-26, not opened.** Written on worktree
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
- **DT-R7. A write holds its relation's `IX` across every yield and
  park** `[quiet-wrong]` (added at DT-S0, §7): taken before the first,
  by the ask a row's claim makes, so no relation-`X` DDL runs between a
  walk's two slices and a resume never binds another relation than the
  one it stopped in.
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

### Marks

**Every row is CLA's proposal adopted on the operator's standing go-ahead
of 2026-11-26 (go-ahead-achieving-milestone)** - *"go ahead until
achieving milestone, I will follow CLA proposal if decision needed"* - and
none is the operator's own mark on the item.

| | adopted | note |
|---|---|---|
| DT-Q0 | (a) | narrowed by DT-Q1's bar to §2.3 and §2.4: an `INSERT` has no walk, so nothing of DT reaches it |
| DT-Q1 | (a), **revised by CLA at DT-S0** | as written the bar read "if the cold-write stall S0 measures is under 1 ms at p99, DT stops". S0 measured two stalls of different size (§7), and one bar over both would stop the walk's yield on the page miss's number. **Read per mechanism**: the cold-write stall gates §2.1 and §2.2, and stops them; the long-walk stall gates §2.3 and §2.4, and does not |
| DT-Q2 | (a) | `PagesPerSlice`, the session's level as a `SELECT`'s |
| DT-Q3 | (a) | no subject: §2.1 is not built. The census's reading stands for whoever builds it (§7, Census B) |
| DT-Q4 | (a) | DT-S3 |
| DT-Q5 | (a) | confirmed by reading (§7) |
| DT-Q6 | **(b), revised by CLA at DT-S3 from its own (a)** | (a) read "each made a `BuildRun` as `PURGE INDEX` is", and read against the tree that holds for none of the three (§7, DT-S3): `PURGE INDEX`'s run holds no transaction, and `PURGE DROPPED COLUMNS` holds a DDL transaction, an epoch gate and a re-run of its own; the catalog's purge and the controller's seeded build are not statements and have no session a `BuildRun` is built on. The three stay in `known-gaps.md`, outside DT |
| DT-Q7 | (a) | a locking read's walk carries an empty slice |

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

### Opened 2026-11-26, and DT-S0: the census

Opened on worktree `dt-write-path-yield` (branch
`worktree-dt-write-path-yield`) from `60dc593af`
(`v3.4.17-150-g60dc593af`), under the operator's standing go-ahead of
2026-11-26 (`raft-marks-2026-11-26.md` §4). Every `path:line` below was
read at `60dc593af`, whose sources are `8274c951`'s (the merge between
them brought documents only). The suite at `60dc593af`, before any
change: 5,224 of 5,224 passed in 80.75 s (`ctest -LE bs-nightly -j 12`,
Debug).

**§1's readings, re-verified.** `kMaxMissReruns = 8`
(`include/kds/server/command_dispatcher.hpp:2276`), `kPageReadThreads = 4`
(`include/kds/storage/page_reader.hpp:59`), `pages_per_slice = 64`
(`include/kds/exec/step_vm.hpp:347`), `kMaxRegisteredReaders = 256`
(`include/kds/txn/manager.hpp:202`), `WriteWalk`
(`src/server/command_dispatcher.cpp:12879`), `CollectTargets` (`:13076`)
and `resumed_targets` (`:12903`) stand as written. The arm is
`ArmMissRefusal` / `NoteMissRefusal`
(`include/kds/storage/miss_refusal.hpp:48-56`).

**Census A - a write's fetches, in order, and what is held at each.**
`src/server/command_dispatcher.cpp` unless a file is named.

| path | fetch, in order | held there | first write |
|---|---|---|---|
| `UPDATE`, every form (`UpdateParsed`, `:13338`) | a scalar subquery of the SET list, run once at compile (`RunScalarSubquery`, `:13396`); the foreign-key parents' descents, once before any row (`:13500-13527`); then the declared unit's ask, where the `WHERE` declares one (`:13548-13560`) | the transaction, the read borrow, no page; a parent's `IS` and row `S` once its check holds them; the declared unit and, through `BorrowChain`, the relation's `IX` | none yet |
| `UPDATE` by key (`write_at`, `:13958`) | the descent, `LocateByTupleKey` (`:9958`), leaf exclusive | the declared unit if one, no page before it | - |
| `UPDATE`, a row (`apply`, `:13588`) | `Qualify` (`:12921`): a spilled cell the `WHERE` reads (`:13290`), the undo chain for a head that failed the `WHERE` (`:12996`); the stage-2 decode's spills (`:13637`) | the leaf, exclusive; the row's unit after `Claim` (`:13019`) | **`NoRefuseWindow::Open` (`:13741`) is the boundary**: after it the assertion reservation (`:13745`), the spill append (`:13775`), `AppendUndo` and `NoteOverwrite` (`:13823-13831`, where the trail moves), the overwrite (`:13838`), the indexes (`:13866`), `HEAP_OVERWRITE` (`:13904`) |
| `UPDATE`, a moved unique key (`clear_unique`, `:13923`) | the index's descents | no page: the walk stopped at the row (`:13715-13722`) | none |
| `UPDATE` / `DELETE`, the walk (`VisitRelation`, `:9443`) | `WalkBtreeLeaves` (`:9347`): one descent to the first or the resumed leaf (`:9364`), then each leaf by `BtreeVisitLeafPage` (`src/storage/btree/btree.cpp:1397`); `WalkHeapPages` (`:9494`) the same by page | **nothing between two leaves**: the leaf's hold ends as its visit returns, and `apply`'s window closes with the row's visit | per row, as above |
| `UPDATE` / `DELETE`, a key list or a unique key (`WriteKeySet`, `WriteKeys`, `:13117`, `:13160`) | a unique key's run collected from its index first (`UniqueKeyRun`, `:13210`), then one descent a key | nothing between two keys | per row |
| a write whose `WHERE` holds a subquery (`CollectTargets`, `:13076`) | a read walk of the whole relation, the sub-chains under it | **no cursor**: `VisitRelation` is handed none, so the pass has no place to stop | none: a read pass |
| `DELETE`, a row (`mark`, `:17202`) | `Qualify`, the complement decode's spills (`:17225`), the reverse foreign-key check (`:17260`) | the leaf, exclusive; the row's `X` | `ReserveDelete` (`:17284`), else `AppendUndo` (`:17304`). **No window of its own**: after `AppendUndo` the one fetch left is the pinned leaf's; where an assertion stands, `AppendUndo`'s fetch follows `ReserveDelete`'s write with no window (not examined further here) |
| `INSERT`, a row (`InsertOneRow`, `:8708`) | the unique check's descents (`:8859-8867`); then, **inside the window that opens at `:8967`**, the clustered descent (`BtreeInsert`, `btree.cpp:1031`: `DescendTo` `:1037`, then `PlaceUnderHold` `:1044`), the indexes (`:9238`) | no page before the window; the key's `X` after its borrow (`:9021` a named key, `:9067` a hashed home, `:9124` an issued id) | a spill at the encode (`:9026`, a trail entry); the mark (`AdmitExplicitRowId` `:9036`, `AllocateRowId` `:9118`: a catalog page, off the trail); the placement, `InsertTupleAt` (`btree.cpp:1079`) |

**Census B - where the trail first moves, and what a re-run already
costs.** A statement is re-runnable while `scope.txn->trail().size() ==
statement_trail_mark_` (`EndWrite`, `:15492-15516`; `ParkedOutcome`,
`:13256`). Four re-runs of a write that wrote nothing exist: a held row's
(R6-5, `AwaitWriteBlock`, `:406`), a relation wait's (`:809`), an
in-flight slot's (DJ-S3, `:1056`, refused before the scope opens) and a
structural refusal's (BA-S14, `:1090`). **An issued id is already burned
by one** (`:9105-9107`: a refused borrow after `AllocateRowId` "burns the
id (K3), which is then free (BD-R4)"), so the mark's bump before an
`INSERT`'s descent - DE-Q7 (a)'s first reason for leaving the write out -
is a page write but not a trail entry, and a re-run past it costs one id.
The no-refuse window is the pool's (BE-Q11), opened before the descent
because the spill and the mark precede it; it is not what makes a
statement re-runnable.

**Census C - who swallows a refused fetch on a write path** (DE-Q7 (a)'s
second reason, DE's list B). `LocateByTupleKey` reads any failure but
`NotFound` as "walk instead" (`:9975-9986`), and every key loop hands a
declined descent to the walk (`DescentDeclined`, `:13234`) - but on a heap
after a row was offered, where it refuses the statement `TxnConflict`. So a refusal
made under a thread-wide arm can be answered by a walk that then writes.
DT's answer is `[design]`, built in DT-S1 if the bar below is met: the arm
is set around the one fetch that may be refused and nothing else (the
locate, the leaf, `DescendTo`), the note is tested where that fetch
returns, and one test stands before a row's first write.

**Census D - the boundary DT-Q1 (a) asks about.** No write path holds a
pin, a latch or a window across a leaf boundary or between two keys of a
list (Census A, rows 5 and 6): the mid-walk park of AO-S3b already ends
the statement's body there and resumes it by key (a btree,
`WalkBtreeLeaves`' fresh descent) or by place (a heap), through
`Session::ParkedWrite` (`include/kds/server/session.hpp:345`) and
`RunWalkWrite`'s resume branch (`:11762-11783`). **Cut and named, by
DT-Q1 (a)**:

- **the collection pass** of a write whose `WHERE` holds a subquery: it
  carries no cursor and a half-collected set has nowhere to wait, so it
  neither yields nor parks;
- **an `INSERT` of many rows** (`VALUES` lists, `INSERT ... SELECT`): no
  resume path exists for it, and §2.3 names the walk, not the row loop;
- **every fetch below the walk**, by §2.5: under the leaf the undo chain, a
  spilled cell, the index maintenance and `DELETE`'s reverse foreign-key
  check, which walks every child relation whole; with no page held a moved
  unique key's check (`clear_unique`) and the foreign-key parents' descents;
- **the locking read's walk** (`RunLockingRead`, `VisitRelation` at
  `:14235`), the one other caller that hands `VisitRelation` a cursor:
  outside DT by DT-Q7, so a slice placed in the walk must be empty for it.

**What a parked write holds, and what it must** `[quiet-wrong]`. A
mid-walk park keeps its transaction, its snapshot and its cursor - with
the trail mark, `NOW()`'s context, the subquery answers and the binding as
data (`Session::ParkedWrite`) - and holds nothing else: the read borrow is
`UpdateParsed`'s and `DeleteInner`'s local (`:13356`, `:17153`), the
statement epoch is `StageStatement`'s (`:1301`), and no catalog memo pin
is held. So §2.3's "borrow and statement epoch held as a suspended
`SELECT`'s are" is not what this park does; DT-R7's `IX` stands in for the
borrow. AO-S3b's park is sound because a statement that parks there has
written a row, and so holds the relation's `IX` through its transaction
(`BorrowChain`, `:15045-15064`), which every relation-`X` DDL waits for. A
write that declares a unit (no `WHERE` at all, or a tuple key window wider
than one key: `DeclaredWriteBorrow`, `:15200`) holds that `IX` from its
declaration, before the walk; one that declares none - DT-S0's walker,
`WHERE c0 = -1` - holds none until its first claim. **A yield or a
cold-leaf park can come before any row is claimed**, and a walk that
holds nothing across it would resume by key into a relation dropped and
created again under its name. So DT-R7 is added to §3: a write takes its
relation's `IX` before its first yield or park, by the ask a row's claim
makes (`BorrowChain`'s, memo check included).

**DT-Q5, confirmed by reading.** An autocommit write's lease is dropped
before the walk (`SnapshotFor`, `:14821-14828`; `RunWalkWrite`'s note,
`:11794-11800`): what holds the horizon is the owned transaction, an
in-flight slot of the core's 1,024, not one of the 256 reader slots. An
explicit transaction is its own registration (`:14810-14813`). A suspended
write takes no reader-lease slot.

### DT-S0: the premise, and DT-Q1's bar read (2026-11-26)

Measured on `dt-write-path-yield` at `60dc593af`
(`v3.4.17-150-g60dc593af`, Release, `cores = 1`, one relation of 900,000
rows in 128,960 pages, four hot sessions of point reads at 1,000 reads/s
in all); `[measured]`
`bench/v3.5.0/results-dt-s0-premise-v3.4.17-150-g60dc593af.md`, driver
`tools/dt_s0_premise.py`. Six cells, two runs each, twelve runs; a seventh
cell cut (`ivwc`, below). About 13 minutes of the stage's 20.

| | the stall | hot sessions' service p99 |
|---|---|---|
| alone (`i`) | - | 530.5 µs, 553.1 µs |
| beside cold point reads, DE built (`iiir`) | parked, off the reactor | 322.3 µs, 336.4 µs |
| beside point `UPDATE`s, the page cache answering (`iiw`) | no device wait | 5,259.5 µs, 5,436.9 µs |
| **beside cold point `UPDATE`s (`iiiw`)** | `io.page_read` on the reactor: mean 91.0 µs and 87.2 µs a read, p99 in [256, 512) µs, 27.9 % and 26.8 % of the core | 5,106.4 µs, 4,789.6 µs |
| **beside one walked `UPDATE` (`ivw`)** | 119-133 ms a walk (min to p95; p50 122-123 ms), no yield, no device | 241.5 ms, 245.0 ms |
| beside one `SELECT` scanner (`ivr`) | yields every 64 leaves | 288.0 µs, 351.7 µs |

**The bar, read per mechanism (DT-Q1 (a) as revised, §4 "Marks").**

- **The cold-write stall is under the bar: §2.1 and §2.2 are not built.**
  One cold read holds the reactor for under 1 ms at p99 (the [256, 512) µs
  bucket); a write makes 1.53 reads on average, and the hold per write was
  not measured. The hot sessions' p99 beside cold writers (5.1 ms, 4.8 ms)
  is not above their p99 beside the same writers with no device in the
  path (5.3 ms, 5.4 ms), but that 5 ms is unattributed and the results
  file claims no p99 delta between the two cells (its §4), so the
  comparison shows no device effect at p99 without ruling one out; the
  device path also carries a memory-limited writeback the warm cell does
  not (its §4.1). What the device adds shows below the bar's
  percentile: the hot p95 is 2,012 µs and 1,535 µs in `iiiw` against
  405 µs and 402 µs in `iiw`. The bar reads p99, so that is stated and not
  acted on. **DT-S1 has no subject**; Census B and C stand for whoever
  reopens it.
- **The long-walk stall is over the bar by two orders: §2.3 and §2.4 are
  built.** A walked `UPDATE` that matches nothing holds the core for its
  whole walk; the hot sessions complete 34.9 and 34.5 reads a second of
  the 1,000 offered, and the same relation's `SELECT` scanner leaves them
  a p99 of 0.29 ms to 0.35 ms.

**Three things the measurement met and DT does not own**, each handed to
`known-gaps.md` at the close:

- the hot p99 near 5 ms beside warm point `UPDATE`s (`iiw`) is not a
  device wait and was not attributed;
- a walked `UPDATE` that writes no row still commits and appended about
  1.05 MB of WAL a walk, and took 2.7 times a `SELECT` walk's time;
- `ivwc` (the walk under a quarter pool with the page cache kept from
  answering) was not measured: the server was killed by the scope's
  memory limit seconds in, after five refusals `buffer pool full ...
  32768 resident (0 pinned, 32753 dirty), nothing reclaimable`. One
  attempt, not repeated; DX's subject (the pool under pressure).

**Not measured**: `INSERT` and `DELETE` as the cold writer, `cores` above
one, `strict` durability, a walk that writes rows, another cold rate (one,
the 2,000/s cap), another device (this one's probe p50 is 47-48 µs;
DE-S0's was 463 µs).

**DT-S0 lands with no behaviour change**: this section, the results file
and its driver. Overhead not measured; measured at the milestone's close.

### DT-S1: not built (2026-11-26)

DT-Q1's bar stops it (above). No code, no cell.

### DT-S0's review (2026-11-26)

`critics-developer`, over the census and the bar's reading, against
`60dc593af`. About 75 citations checked; all point at the right line.

**Applied, by the reviewer in this file**: Census D's cut list split
fetches under the leaf from those with no page held (a moved unique key's
check and the foreign-key parents are the second kind); `RunLockingRead`'s
walk named as the one other caller that passes a cursor; the relation's
`IX` already held by a write with a declared unit, so DT-R7's case is an
undeclared `WHERE`; four re-runs of a write that wrote nothing, not three
(DJ-S3's slot wait); Census A's missing asks; `DescentDeclined`'s refusal
on a heap; three misquoted numbers (six cells in two runs, the read rate
as a total, a walk's 119-133 ms); and the bar's reading bounded to what
the results file claims.

**Kept as findings, none changed in code by this stage**:

- **DT-Q1's revision was made after the numbers were in**, and what stops
  §2.1 and §2.2 is one host's measurement: a device whose probe p50 is
  47 µs, one cold rate, `cores = 1`, `relaxed`. On DE-S0's device (463 µs
  a read) one cold read is near the bar or over it. `Estimate: a cold
  write's read there is 0.5 ms to 1 ms at p50 (basis: DE-S0's probe and
  its 540 µs mean per read; confidence: medium).` The revision is CLA's
  and is the first item of DT's report to the operator.
- **The fetches under a leaf are named and not sized**, against §2.5's
  "DT-S0 sizes them". The heaviest is a parent `DELETE`'s reverse check
  (`CheckNoChildrenBeforeDelete`), which walks a child relation under the
  parent leaf's exclusive hold: no slice boundary splits it, so one row of
  such a `DELETE` can hold its core for a whole child walk. Handed to
  `known-gaps.md` at the close.
- **A `DELETE` on a relation with an assertion writes its reservation
  before `AppendUndo` with no window around the two.** Read: a refusal at
  the undo append fails the statement, and the transaction's abort gives
  the reservation back (`AbortOwnedScope`, or the client's `ROLLBACK`), so
  nothing is left half done. Not verified by a cell. Handed to
  `known-gaps.md` at the close as unverified.

**Rejected**: none.

### DT-S2: a write's walk gives its core back (2026-11-26)

§2.3 alone; §2.2 is not built (DT-S0). `[source-read]` at this stage's
commit, `src/server/command_dispatcher.cpp` and
`include/kds/server/command_dispatcher.hpp`.

**Built.** A walked `UPDATE` or `DELETE` stops between two leaves every
`PagesPerSlice` leaves of its session's level (64; 16 for `analytic`), and
a key loop (a list on the tuple key, a unique key named whole) between two
descents on the same count. The stop is the mid-walk park's: the walk
leaves its place in the `WalkCursor`, the body returns, the statement's
transaction, snapshot and cursor go to `Session::ParkedWrite`,
`DispatchAsync` yields once (`AwaitWalkPause`) and dispatches the line
again, and `RunWalkWrite`'s resume branch carries on. `Dispatch` resumes
where it stands, so the suite and the simulator cross the same
boundaries.

- **The place** (`WalkSlice`, `WalkBtreeLeaves`, `WalkHeapPages`). A
  btree's walk resumes by key above the highest key of the last leaf it
  finished, read under that leaf's hold at its last slot, and pauses only
  where it has one above the key it resumed at; a heap's at the next
  page's first slot, after a page finished in the same call; a key loop at
  its next key. The leaf count is the statement run's, not the call's: a
  walk is called again for each row a moved unique key stopped it at.
- **DT-R7** (`PausedWalk`). Before every pause the transaction asks the
  relation's `IX` by `BorrowOrWait`, a re-entrant grant once a row was
  claimed and the first intention - memo check included - where none was.
- **DT-R3** (`UpdateParsed`, `DeleteInner`). A resumed run skips its
  single-key arm. Without that, a statement whose key's descent declined,
  whose walk wrote the key's row and then paused, wrote the row again on
  the resume: the mutant answers `UPDATED 2`
  (`KeyedWritePauseTest.AResumedWalkDoesNotOfferItsSingleKeyAgain`).
- **DT-R1.** Nothing new holds a page: the body has returned before the
  task yields, and the suspend audit (`exec::InstallSuspendAudit`,
  `ExecSuspendAuditTest`) is what aborts a debug build that suspends under
  a pin, a span or a window. No new cell plants one.
- **DT-R5.** The yield is a bare `sched::Yield`, booked by the scheduler
  under `sched.run_queue`, as a `SELECT`'s slice boundary is.
- **A pause is not logged as a statement** (`DispatchAndStage`), and
  `CommandDispatcher::write_walk_pauses()` counts them for a test.

**Not built, and stated**: the collection pass of a write whose `WHERE`
holds a subquery (no cursor; the write walk after it is sliced); an
`INSERT` of many rows; a locking read's walk (DT-Q7 (a)); a sim seed class
of its own - the corpus drives `Dispatch`, which pauses and resumes inline
for every walk past one slice, and no knob shortens the slice for it.

**Cells.** `tests/dt_write_yield_test.cpp`, twelve, each over a heap and a
btree: a walked `UPDATE`'s pauses and its rows written once; the default
slice; a second writer's inserts between two slices (DT-R3); the trail
inside a transaction; `DELETE`; a key loop; the relation held by a paused
walk that claimed nothing (DT-R7); `Dispatch`'s inline resume; a held row
after pauses; `RETURNING`; a moved unique key; collected targets. And
`KeyedWritePauseTest` in `tests/key_set_test.cpp`. Two cells of
`tests/walk_slice_rig_test.cpp` were adapted: their writer walks, so it
now yields and no longer ends inside the reader's first suspension; the
reader runs at `analytic` so it is still walking when the writer ends,
which is those cells' precondition. Their data assertions are unchanged.

**Review** (`critics-developer`): no correctness defect found in the
pause and the resume; seven mutations built and run. **Applied**: the
test file's "heap" tables were btrees (a table that names no form is
one), so the heap's pause had no cell - fixed by the reviewer; one stop
check for the held-row park and the pause (`WriteWalk::StopOutcome`); the
high key read only at the leaves a pause can follow; two dead stores; the
null-cursor guards made asserts; `Dispatch` ends only a stranded park it
left itself; a cell for the single-key arm's guard, the one mutation no
cell caught; and four more cells (a held row after a pause, `RETURNING`,
a moved unique key, collected targets). **Found by the added cell, not by
the review**: a walk called again per row never reached a slice boundary,
because its leaf count was the call's - a statement that moved a unique
key on every row took no pause. **Rejected**: (1) a heap key loop whose
descent declines after a pause is refused `TxnConflict` where a walk from
the head would answer - kept, because the fix has to reset a cursor that
holds a key and no page, the shape DR-S3a's review found answering a
short count, and no cell can force a declined descent there; it is a
refusal, never a wrong answer, and goes to `known-gaps.md`. (2) Asserts
in place of `PausedWalk`'s clearing of a stale blocker and lock wait -
kept as clears: `WriteWalk::Claim`'s own note says a forward foreign-key
check may leave a holder on the member.

**New refusals a pause can make**, stated: the first-intention memo check
at a pause that claimed nothing (a catalog write between the statement's
resolution and its first pause re-runs it, or under `Dispatch` answers
`TxnConflict`); a transaction at `max_locks_per_txn` that claimed nothing
is refused at its first pause; a subquery's relation dropped or renamed
between two slices refuses the statement mid-walk, as it did at a held
row's park.

**Suite**: 5,236 of 5,237 (`ctest -LE bs-nightly -j 12`, Debug), twice.
The one failure both times is
`DurableKickRigTest.APeersGroupCommitIsKickedByTheWriter`, which is not
this stage's: run twelve at a time, it fails 23 of 60 at `60dc593af`
(built from `git archive` outside the tree) and 27 of 60 here. Overhead
not measured; measured at the milestone's close.

### DT-S3: a cancel taken where a write gives its core back (2026-11-26)

§2.4, and DT-Q6 revised to (b).

**Built** (`CommandDispatcher::CancelPausedWalk`). At a write's pause the
session's cancel flag is taken - before the yield and after it in
`AwaitWalkPause`, and before the inline resume in `Dispatch`'s loop - and
the park's scope is ended through `RefuseParkedWrite` with `Cancelled`: an
owned transaction is aborted, so no row the statement wrote stays; inside
an explicit transaction the rows stay until the client's `ROLLBACK` and
the transaction is failed, as a walking `SELECT`'s cancel leaves it. One
cancel, one statement. A `C_CANCEL` (through the cancel registry) and a
connection closed under its statement (`TcpServer::CloseClient`) set that
one flag, so a dropped connection's long autocommit write is now aborted
at its next pause instead of applied in full. A write that takes no pause
- an `INSERT`, a write by one key, a walk or a key loop under one slice -
looks at no flag.

**DT-Q6, read against the tree, and why (a) is not adopted.** (a) was
"each made a `BuildRun` as `PURGE INDEX` is".

- `PurgeIndexRun` holds no transaction: each slice asks the relation's
  `IX` under a read holder, try-only, re-reads the catalog and writes
  images at `kNoTxnId`.
- `PURGE DROPPED COLUMNS` (`HandlePurgeDroppedColumns`) runs in a DDL
  transaction it writes its flag in, under an `IX` held to the decide, an
  epoch gate (`purge_wait`) and `DispatchAsync`'s re-run of the statement.
  Made a run it is `IndexBuildRun`'s shape - a write scope held across
  yields, the DDL undo hook uninstalled and installed, a destructor that
  aborts - and its gate is `[quiet-wrong]` (CD §3: a zeroed cell read as
  NULL, a released spill followed). Not `PURGE INDEX`'s shape.
- The catalog's delete-mark purge (`Catalog::PurgeSettledDeleteMarks`,
  core 0's tick) and the controller's seeded build
  (`CabinOptimizerExecutor::BuildSeededSets`, the tick's timer callback)
  are not statements: no session, no statement frame, and
  `BuildRun(CommandDispatcher&, Session&)` is built on both.

No measurement of DT's sizes any of the three. So the three stay where
they are in `known-gaps.md`, with this reading beside them, and DT builds
none.

**Cells** (`tests/dt_write_yield_test.cpp`, five more, a heap and a btree
each): an autocommit `UPDATE` and `DELETE` cancelled at a pause leave no
row written, no unit held and no transaction open; inside a transaction
both leave their rows on the trail and the transaction failed; a walk
under one slice and a write by one key take no cancel; the synchronous
path takes one; a key loop's pause takes one. **No cell drives `C_CANCEL`
or a closing connection over the wire**: both reach the write as the
session's flag, which the cells set directly.

**Review** (`critics-developer`): no defect found; nine mutations built,
five caught, four not - the two checks around the yield (either alone
still takes the cancel within a turn), the core guard (the fixture has
one core) and a clear that did nothing. **Applied**: that clear deleted;
two stale comments (`TcpServer::CloseClient`'s, by the reviewer, and the
session flag's); the transaction cell shows the rows stayed on the trail
and runs `DELETE` too; the no-pause cell walks a relation under one slice
where it had written one key; a cell for a key loop's cancel; the
autocommit cell shows another writer takes a row at once. **Rejected**:
logging the cancelled write through `LogStatement` - it needs the
statement's start carried to the pause, and what the log gets today is
`RefuseParkedWrite`'s warning with no statement text; stated in
`known-gaps.md`.

**Suite**: 5,241 of 5,242, and 5,241 of 5,241 in the reviewer's run of
the tree before the cells above were added; the one failure is
`DurableKickRigTest.APeersGroupCommitIsKickedByTheWriter` again (DT-S2).
Overhead not measured; measured at the milestone's close.

### DT-S4: the close (2026-11-26)

Two measurements, a remedy between them, and CLA's reading of the second.

**The first close measured a regression, and DT did not close on it**
`[measured]` (`bench/v3.5.0/results-dt-close-v3.4.17-153-gd172085e8.md`,
A `60dc593af` against B `d172085e8`, `cores = 1`, `relaxed`). The effect
was there: beside a walked `UPDATE` matching nothing the co-located reads'
p99 fell from 245.2 ms and 265.1 ms to 10.26 ms and 1.62 ms. DT-R4 held:
B less the control was -70 ns to +40 ns on the point statements. **But
under the default checkpoint cadence B stalled for seconds** (the reads'
max 1.39 s and 7.66 s), and the window's log went from 169 MB to 3,020 MB,
its page images from 20,672 to 368,312. With `checkpoint_interval_ms = 0`
B was clean (p99 332.4 µs).

**The cause, by reading** `[source-read]`. A write walk fetched every leaf
it passed `PageAccess::kWrite`, and a write fetch marks the frame dirty
and logs the image it owes (`wal.md` §10) whether or not a row is written.
Before DT that cost one image a leaf a checkpoint interval, because the
walk held its core and the cadence checkpoint could not run inside it. A
walk that yields hands every leaf it passed to the checkpoint between two
slices; each copy owes an image at the walk's next pass, and the log's
sync gate and the checkpoint's writeback take the core by turns.

**The remedy** (`6e43ec8c0`): **a write walk looks at a leaf before it
takes it for writing.** Under a shared hold it asks, slot by slot, the
function the write judges a row with (`WriteWalk::Qualify`, passed as
`LeafProbe`; `LookAtLeaf`) until one row would be written, and only then
takes the leaf exclusive, judging every row again under that hold; a leaf
with no such row is passed over, neither dirtied nor imaged. The look
decides which leaves are written through and never which rows. The rows a
resumed walk is past are skipped by one rule for the look and the visit.
By design this ended what eight cells pinned - that a write walk holds
every leaf exclusive, which `KeySetWriteHoldTest` (seven cells) used to
tell a walk from a descent and
`LockingReadRigTest.NoPageIsDirtiedAndNoRecordIsAppended` stated; the
watching store now records a leaf pinned in any mode, and the rig cell
states the new fact: a walk that writes no row dirties no leaf.

**The remedy's review** (`critics-developer`): no wrong answer found.
**Applied**: the key-set cells, which after the first adaptation no longer
told a descent from a walk; the look's and the visit's two skip rules made
one (`finished`); `LookAtLeaf` moved out of the header; the rig cell's
write asserted as `UPDATED 2`. **Not caught by any cell**, stated: the
look asking a resumed walk's finished rows, and a wanted leaf's sibling
taken from the look instead of from the write hold.

**The second close** `[measured]`
(`bench/v3.5.0/results-dt-close-v3.4.17-154-g6e43ec8c0.md`, three arms: A
`60dc593af`, B `d172085e8`, C `6e43ec8c0`; `cores = 1`, `relaxed`, the
default cadence, two runs a cell, 900,000 rows in 128,960 pages, four
sessions asking 1,000 point reads a second):

| | A | B | C |
|---|---|---|---|
| reads beside a walk writing nothing: p99 | 253.5, 263.4 ms | 9.0, 9.7 ms | **320.7, 365.0 µs** |
| the same: max | 268, 265 ms | 3.44, 4.12 s | **1.53, 2.85 ms** |
| the same: reads answered a second | 34.5, 26.3 | 488.6, 838.2 | **1,000.2, 1,000.2** |
| the same: log, page images | 171, 130 MB; 20,800, 15,872 | 2,081, 4,943 MB; 253,756, 602,676 | **0; 0** |
| reads beside a walk writing 50,000 rows: p99 | 0.66, 3.39 s | 344, 687 ms | **51.1, 50.9 ms** |
| that walk alone: walks finished, walks over 1 s | 30, 43; 11, 9 | 4, 21; 2, 3 | **83, 82; 0, 0** |
| that walk alone: page images | 1,152, 1,792 | 142,115, 126,336 | 57,094, 57,094 |
| keyless walked `UPDATE`, 100 rows of 100,000, btree: server CPU against A | | +430.5 µs (+13.1 %) | **+212.5 µs (+6.4 %)**, 15 of 15 rounds |
| the same on a heap | | +21.1 µs (+0.6 %) | **-77.3 µs (-2.4 %)** |

- **The stall is gone**: no walk of C's is over 1 s in its nine walker
  cells (A: 25 in eight, B: 28), no hold reaches 100 ms, and the walk's
  own best time is A's within 0 ms to 3 ms where B's was 10 ms above it.
- **DT-R4 holds on C**: the eight point statements are -10 ns to -240 ns
  against A, the control (a second copy of A) within 50 ns. A point
  `SELECT`, which no line of DT reaches, moved by the same -3 %: the two
  binaries differ by that much and the file does not assign it.
- **What C costs, measured**: the +6.4 % above on a btree; and a walk that
  writes now has its pages copied by the checkpoint between slices, each
  copy imaged again at the next write - 9 % to 21 % more log a walk in a
  loop writing the same 7,150 leaves about 70 times in 20 s.
- **What is left and not located**: the reads' p99 beside the writing walk
  is about 50 ms (13 to 67 times under A's), a plateau no wait event
  accounts for.
- **Not measured**: a walk whose written rows are spread over every leaf
  (the look's worst case; the relation admits no such predicate without a
  reload), `cores > 1`, `strict`, an `analytic` session's slice, a write
  driven by a list, a cancel's latency, a pool smaller than the relation.
- **The host was not quiet**: another session built and ran its suite in
  bursts; the cells it overlapped were discarded and run again, 13.7
  minutes were spent holding for it, and the cells of the first pass were
  checked at their ends and not sampled between.

**CLA's reading, adopted on the standing go-ahead and not the operator's
mark**: the regression the first close measured is gone, and what C costs
is the price of the mechanism - a statement that gives its core back pays
for the giving, and a checkpoint that can now run does its work - stated
in `known-gaps.md` ("What a write's pause costs and what it can refuse")
with its numbers, not a regression that stops the close. **The other
reading is open to the operator**: 6.4 % on a walked `UPDATE`, or 9 % to
21 % of log on a repeated writing walk, as a regression; DT's code is
three commits (`ad74b4c44`, `d172085e8`, `6e43ec8c0`), and
`WriteSliceFor` returning an empty slice turns every pause off and
leaves the look.

**DT closes narrow**: §2.3 and §2.4 built; §2.1 and §2.2 stopped by
DT-Q1's bar as CLA read it at DT-S0, on one host; DT-Q6's three walks left
outside. The page miss of a write is owned by no order.

**Documents**: `sched.md` §3 (the write's pause, the look), `txn.md` §5
(the relation `IX` across a pause), `protocol.md` §10 (the cancel),
`CLAUDE.md`'s two rows, `manual/sql/sql.md`, `known-gaps.md`. `page.md`
§12 is unchanged: nothing of a write's page miss was built.

**Suite** at `6e43ec8c0`'s build inputs (`scripts/test.sh`, the range the
push hook runs, serial, 634.5 s): **5,242 of 5,242 passed, 0 failed**, two
of them reporting skipped (`CatalogPageCeilingTest`, two cells) and two
more disabled and not run (`StrictMarkerSnapshotRigTest`, two cells). The
run before it on the same tree (`ctest -j 12`) was 5,240 of 5,242, both
failures the two cells that fail at `60dc593af` too
(`DurableKickRigTest.APeersGroupCommitIsKickedByTheWriter`,
`SetExprTwoCoreRigTest.TwoCoresIncrementingOneRowLoseNoIncrement`;
`known-gaps.md`, Testing). The close's commit after it brings documents,
`bench/` and `tools/dt_close.py` only, no build input.