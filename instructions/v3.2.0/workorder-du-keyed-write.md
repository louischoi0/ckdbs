# Work order DU - a write that finds its rows by a key: a bound on the tuple key, an index, and what a subquery leaves

Status: **OPEN since 2026-11-26.** Written on worktree
`known-gaps-milestone-orders` (branch
`worktree-known-gaps-milestone-orders`) from `8274c951`
(`v3.4.17-148-g8274c951e`) on the operator's word of 2026-11-26
(`raft-marks-2026-11-26.md` §3). **Opened the same day** on worktree
`du-keyed-write` (branch `worktree-du-keyed-write`) from `60dc593af`
(`v3.4.17-150-g60dc593af`), on the operator's word of 2026-11-26
(`raft-marks-2026-11-26.md` §4): DU-Q0..Q6 are adopted as CLA proposed,
(a) in each, **on the operator's standing go-ahead of 2026-11-26
(go-ahead-achieving-milestone)** - none is the operator's own per-item
mark - DU-Q2 (a) with its reading fixed against DU-R4 and DU-Q3 (a) with
its premise corrected (§4, §7), and DU-Q7 (a), an item CLA added at
DU-S2, adopted the same way. A `[source-read]` claim with a `path:line`
in §0-§2 was read at `8274c951`, and one in §7 at the commit its entry
names; a claim taken from `known-gaps.md` carries that entry's own
verification commit and is not re-verified here.

Tags: `[operator]`, `[source-read]`, `[measured]`, `[design]`,
`[quiet-wrong]`, as in DT's order.

Related: `docs/spec/parser-v2.md` I10, §7; `docs/spec/index.md` §9, IX2,
IX4b; `docs/spec/txn.md` §5; `docs/inflight/known-gaps.md`, "Key sets
(CU)"; `docs/pending/keyed-write-beside-a-subquery.md` (deferred by the
operator on 2026-11-17; DU does not decide it, DU-Q5);
`instructions/v3.2.0/workorder-cu-key-set-access.md` (CU-Q5);
`instructions/v3.2.0/workorder-dr-primary-key-over-keystone.md` (a write
by a unique key named whole).

## 0. What DU is

`[source-read]` Today a write descends only for an equality or a list on
the tuple key, or a unique key named whole; every other `WHERE` walks the
relation (`CLAUDE.md`, "Query language"; `known-gaps.md:3010`). `[design]`
DU gives `UPDATE` and `DELETE` two more ways to find their rows - a
bounded walk under a tuple key range, and a non-unique index's equality
prefix or range - and writes the rows found under the same units the walk
of the same `WHERE` takes. No answer changes; the rows visited do.

## 1. The tree as read

`[source-read]` from `docs/inflight/known-gaps.md` at `8274c951`:

- **A write finds its rows through no index and no Cabin** (`:3010`):
  `UPDATE ... WHERE owner IN (1, 9)` walks the relation whatever index
  `owner` has. CU-Q5 names the point-write target; an index-driven write
  was no stage of CU.
- **A write over a bound on the tuple key walks the relation**
  (`:3003`): `WHERE id BETWEEN 10 AND 20` declares its lock window off
  the bound and visits every leaf - "a write's walk has a resume position
  and no end".
- **A write whose `WHERE` holds a subquery walks its relation twice**
  (`:2995`): a collection pass, then every slot of every leaf under a
  write latch (`WriteWalk::CollectTargets`), because a write evaluates no
  subquery under the leaf it holds exclusive. Deferred on the operator's
  word (`docs/pending/keyed-write-beside-a-subquery.md`).
- **A list on a relation the write only reads is keyed already**
  (`:3007`).

`[source-read]` in the code at `8274c951`: the walk is
`CommandDispatcher::WriteWalk` (`src/server/command_dispatcher.cpp:12879`),
its point arm `LocateByTupleKey` reported as `PointWrite`
(`:12881-12888`), its collection pass `CollectTargets` (`:13076`, called
at `:14041` and `:14231`); the read side's correlated index probe is
`CorrelatedIndexProbeOf` (`src/exec/step_compiler.cpp:1665`).

## 2. The mechanism `[design]`

1. **A bounded walk.** Where the `WHERE` bounds the tuple key, the walk
   seeks to the low bound and ends at the first leaf past the high bound.
   The lock window is the one declared today; only the leaves visited
   change. On a heap the bound is on the Keystone and the range is the
   Keystone index's.
2. **An index-driven write: collect, then write.** Where the read
   ladder would take an index for the same `WHERE` - an equality prefix,
   a list on a prefix column, a range after the prefix - the statement
   first reads the index at its view and collects the tuple keys, then
   writes each by the existing point arm, the whole `WHERE` checked again
   on the row found. Collect-then-write is what keeps a statement that
   moves the indexed column from meeting its own new entries.
3. **The units.** The statement takes what the walk of the same `WHERE`
   takes: the relation `IX`, the declared window or fence where the walk
   declares one, a row unit per row written. DU-S0's census reads what a
   walked write without a tuple key bound declares; if it declares the
   whole key space, an index-driven write does too, and DU states that
   it saves reads and no waits (DU-Q3).
4. **Which arm.** The read ladder's rank chooses; an administrator's plan
   for the shape restricts it as it does a `SELECT`'s (DU-Q4).
5. **`ANALYZE`** names the arm a write took.

## 3. Rules

- **DU-R1. The rows written are exactly the rows the walk would write**
  `[quiet-wrong]`. An index entry finds a candidate and never decides a
  row: every candidate is qualified on the row at the slot, under the
  walk's own rule (IX2), and a row with two entries is written once.
  The byte-for-byte index contract suite gains a write family: every
  write shape run with `indexes = on` and `off` leaves identical
  relations.
- **DU-R2. A row the collection missed is a row the walk's view could
  not see.** The collection reads at the statement's view; DU-S0 states
  for each isolation level why an index read at that view and a walk at
  that view qualify the same set, or DU does not build that level.
- **DU-R3. The lock units are a superset of the walk's or equal to
  them**, never fewer `[quiet-wrong]`.
- **DU-R4. A shape DU does not serve walks as today**; nothing is
  refused that was answered.
- **DU-R5. No keyword, format, record kind or fingerprint moves.**
- The standing rules, as DT's order lists them.

## 4. Items for the operator (DU-Q), CLA's proposals

| | item | options | CLA proposes | mark |
|---|---|---|---|---|
| DU-Q0 | Is DU opened, and with what scope? | (a) §2's bounded walk and index-driven write; (b) the bounded walk only; (c) both and a Cabin-driven write | (a): a Cabin set is authoritative only for values observed, and a write that observes nothing gains little | (a), adopted on the standing go-ahead of 2026-11-26 |
| DU-Q1 | Does DU-S0 gate? | (a) a premise with a bar: an `UPDATE` by a non-unique index key at 1 M rows measured against its walk; under a 5x saving DU stops at the bounded walk. (b) built regardless | (a) | (a), adopted on the standing go-ahead of 2026-11-26. **The bar is passed**: 12.9x and 308x (§7, DU-S0) |
| DU-Q2 | The collection's size | (a) bounded by `max_locks_per_txn`, as a walked write's row units are; past it the statement is refused as today. (b) falls back to the walk | (a) | (a), adopted on the standing go-ahead of 2026-11-26, **its reading fixed at DU-S0 against DU-R4**: a first collection of more than `max_locks_per_txn` index entries is not written from (entries, not distinct keys, since DU-S1: counting entries is what lets the read stop at the bound), and the statement is answered as today - by its walk, which the row units refuse where today's walk is refused. A collection is a superset of the rows written (IX1), so refusing on its size would refuse a statement the walk answers |
| DU-Q3 | Where the walk declares the whole key space | (a) the index-driven write declares the same and saves reads only; (b) an index-key range lock is designed - a new unit kind | (a); (b) is a lock-model change for its own order | (a), adopted on the standing go-ahead of 2026-11-26, **its premise corrected at DU-S0**: only a write with no `WHERE` declares the whole key space, and it has no index arm; a write whose `WHERE` bounds no tuple key declares nothing and takes a row unit per row written, and the index-driven write takes the same (§7). No unit is designed |
| DU-Q4 | Plans | (a) the ladder's rank, and an administrator's one-relation plan may pin a write's arm as it does a read's; (b) rank only | (a) | (a), adopted on the standing go-ahead of 2026-11-26 |
| DU-Q5 | A write whose `WHERE` holds a subquery | (a) stays outside DU until the pending entry is ruled on; (b) the operator rules now and DU carries it | (a) | (a), adopted on the standing go-ahead of 2026-11-26 |
| DU-Q6 | `PURGE` by list or range | (a) outside DU; (b) inside | (a) | (a), adopted on the standing go-ahead of 2026-11-26 |
| DU-Q7 | **On a heap a keyed write meets its rows in key order and its walk met them in page order.** A check made per row against the rows as they stand - a unique key the statement shifts, an assertion's bound - can therefore answer one order and refuse the other (added by CLA at DU-S2, from its review; `tests/keyed_write_test.cpp`, `OnAHeapAnOrderDependentWriteIsJudgedInKeyOrder`) | (a) accepted and stated: both orders are executions the statement allows, a btree's walk is already in key order, and CU-S3's list and DR-S3a's unique key have written a heap in key order since they landed; (b) a heap's collected keys written in the walk's order - not computable from the keys, since the walk follows the page chain and a rowid does not say where its page is in it, so it costs a walk of the chain; (c) no keyed arm on a heap | (a): the alternative that keeps the walk's order is the walk | (a), adopted on the standing go-ahead of 2026-11-26 (go-ahead-achieving-milestone). **It narrows DU-R4 on a heap**: a statement whose rows are judged against each other may be refused where its walk was answered, and answered where it was refused |

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **DU-S0** | The census: every unit a walked write takes, by `WHERE` shape and isolation level; DU-R2's argument per level; the read ladder's entry points a write can reuse. The premise measured (DU-Q1). No behaviour change | a results file, the census in §7 |
| **DU-S1** | The bounded walk, btree and heap | cells: the leaves visited; the window unchanged; a concurrent insert inside and outside the bound |
| **DU-S2** | The index-driven write for an equality prefix and a list | the contract suite's write family; a statement that moves its own key; two cores |
| **DU-S3** | A range after the prefix; `ANALYZE`; plans as DU-Q4 marks | cells per shape; the sim drawing indexed writes |
| **DU-S4** | The close: overhead A/B on a keyed and a walked write, the specs, `known-gaps.md` | the operator's word on landing |

## 6. What DU does not do

- No index-only write, no write through a view, no new lock unit.
- No change to which rows qualify, to `ON CONFLICT`, or to a foreign
  key's checks.
- Nothing for a write's subquery (DU-Q5) or for `PURGE` (DU-Q6) unless
  marked.

## 7. Stage log (CLA, appended as stages land)

### Written 2026-11-26

On `known-gaps-milestone-orders` from `8274c951`, on the operator's word.
No stage is opened and no code is written. **Not reviewed**: the
`critics-developer` review was started and stopped before it reported,
so no line reference or reading in this order has a second reader behind
it.

### DU-S0 - the census and the premise (2026-11-26)

On `du-keyed-write` at `60dc593af` (`v3.4.17-150-g60dc593af`), read from
`src/server/command_dispatcher.cpp` and `src/exec/step_compiler.cpp`
unless a line names another file. No behaviour changed.

**The units a walked write takes, by `WHERE` shape** `[source-read]`. An
`UPDATE` and a locking read ask `U`, a `DELETE` `X` (`:13599`, `:14198`,
`:17235`); the shape decides the unit, not the mode.

| `WHERE` | declared before the walk (`DeclaredWriteBorrow`, `:15200`) | per row (`WriteWalk::Claim`, `:13019`) |
|---|---|---|
| none | the range `[0, kIdSpaceEnd)` | nothing: the declaration covers the row |
| a tuple key window of more than one key (`FoldTupleKeyWindow`: `=`, `<`, `<=`, `>`, `>=`, `BETWEEN` on the tuple key by name, one-sided included) | the range `[lo, hi)` | nothing |
| a window of one key; a list on the tuple key; a unique key named whole; a `WHERE` none of whose conjuncts bounds the tuple key | nothing | the row's tuple unit, asked once the row has qualified, in the order the rows are met |
| an empty window (`lo >= hi`) | nothing | nothing qualifies |

Above either column the relation's `IX`, taken with the statement's first
unit (`BorrowChain`). Beside them and unchanged by how a row is found: a
unique key's `kIndexKey` unit where the `SET` moves one, a foreign key
parent's `IS` and `S`, an assertion's reservation. **A window is folded
from the raw conjuncts by column name** and reads no embedded primary key
(`a = 1` on a key embedded as the Keystone id declares nothing).

**The isolation level changes no unit.** It changes two verdicts and one
wait: which version selects a row (`SnapshotVersionDecides`, `:13320`;
`txn.md` §5, F4), what a refused declaration's wait is worth
(`RepeatableReadWait::kFutile`), and whether a statement re-run after a
wait mints a view. All three are asked of a row or a unit after it is
found, by code the point arm and the walk share (`Qualify`, `Claim`,
`BorrowOrWait`).

**DU-R2, argued once for both levels.** Let `V` be the statement's view
(`READ COMMITTED` and autocommit: minted at the statement's start;
`REPEATABLE READ`: at `BEGIN`; a resumed statement keeps it), and let the
collection read the index's entries after `V` was minted, holding no page.

- *A row the walk would write is collected.* The walk writes a row only on
  its page's version, whose writer is this transaction or one `V` sees
  (`txn.md` §5's table; a version reached through undo is never written).
  That version was on its page before `V` was minted or before this
  statement began, and its index entry was appended under the same leaf
  hold (`MaintainIndexes`, IX2) - **for an index that is not building**:
  an online build's backfill appends later, and the argument rests on no
  write choosing a building index, as no read does (`IndexProbeOf`,
  `step_compiler.cpp:1430`; `WriteIndexKeyOf`, `:2657`). It is the row's
  head from then until the write, and a head's entries are owed (IX2a), so
  no purge removes one.
  The entry lies inside the probe's bounds because the version passes the
  `WHERE` and the bounds are the read ladder's own superset of it (IX1).
- *A row the walk would wait on or refuse through F4 is collected.* The
  snapshot's version passes the `WHERE` and its superseder is not resolved
  for every reader while this transaction's view is live (`live_` bounds
  the horizon, `manager.cpp:672`), so its entry is owed too.
- *Every collected row is judged where it is written*, by `Qualify` and
  `Claim` on the slot the descent finds: the walk's verdict for a walk
  that reached the row at that moment. **The whole `WHERE` is asked
  there**, which is the point arm's `Qualify` and not the collection
  pass's: a row offered as a collected target (`WalkCursor::targets`)
  returns before its `WHERE` is tested (`:12969`), so the keys an index
  names - a superset (IX1) - must never be carried as targets.
- *A row the collection missed* therefore has no version passing the
  `WHERE` that `V` sees or that this transaction wrote: at most a head by
  a writer `V` cannot see, whose entry was appended after the collection
  passed it. A walk that met that head would wait on its writer or be
  refused; the collection does not meet it. **This is the one
  difference: a wait forgone, and with it the row the wait could buy.**
  The walk asks that row's unit and parks; where it has written nothing
  yet, `READ COMMITTED` runs it again under a fresh view once the writer
  commits, and that run writes the row. The index-driven statement
  neither waits nor writes it. Both are answers its level allows - under
  the statement's own view the row does not match, and PostgreSQL's
  index scan answers as the collection does - so DU-R1 is read as the
  order's DU-R2 words it: the rows written are the rows the statement's
  view can see matching. DR-S3a's unique-key run shares it.

**A statement that moves its own key** is covered by the order of the
work and by the resume: the first collection ends before the first write,
the keys are written ascending, and a resume collects again and starts at
the key it stopped on (`WriteWalk::WriteKeys`, `:13160`) - every row this
statement wrote sorts below that key, and an entry it appended names a row
it wrote.

**DU-R3.** The index-driven write reaches `Claim` through the same row
function as the walk and the declaration is taken before either, so for
every row both meet the unit is the walk's. The one row the walk meets
and the collection does not (above) is a unit the walk asks and is
refused, so the index-driven write holds no unit the walk would not hold
and none fewer among the rows it writes. **One order differs, on a heap**: the
walk meets rows in page order and a key loop in key order, as CU-S3's list
and DR-S3a's run already do there.

**DU-Q3's premise, corrected.** A walked write whose `WHERE` bounds no
tuple key declares nothing; only a write with no `WHERE` declares the
whole key space, and it names no index. So no index-driven write widens or
narrows a declaration, and it saves the leaf holds as well as the reads:
the walk takes every leaf exclusive, the point arm the leaves its rows are
in.

**The read ladder's entry points a write can reuse** `[source-read]`,
`[measured]` on a Debug build of `60dc593af` by `ANALYZE`:

| read arm | function | a `SELECT` takes it for | a write today |
|---|---|---|---|
| tuple key range, btree | `TupleKeyRangeOf` (`step_compiler.cpp:1059`) | both bounds present, strict or not: `id BETWEEN 3 AND 6` is `Range [3, 6]`. `id > 3` alone is `Scan` | walks every leaf |
| embedded key window | `EmbeddedKeyWindowOnStep` (`:1232`) | a prefix or a range of an embedded key | the whole key pinned descends (DR-S3e); a prefix walks |
| a heap's Keystone range | `IndexChoiceOver` over the Keystone index | `>=` and `<=` both: `id BETWEEN 3 AND 6` is `IndexRange`. `id >= 3 AND id < 6` is `Scan` | walks every page |
| index equality prefix, a list, a range after it | `IndexProbeOf` (`:1424`), `IndexChoiceOver` (`:1294`) | `owner = 1`, `owner IN (1, 2)` (`key_set=2`), `owner BETWEEN 1 AND 2`; `owner > 1` is `Scan` | a unique index pinned whole descends (`WriteIndexKeyOf`, `:2651`); every other shape walks |
| the entries between two bounds | `index::IndexSeekLeaf`, `index::IndexVisitFrom` (`RunIndexStep`, `step_vm.cpp:1493`) | phase 1 of an index step | `IndexRunTupleKeys` reads one key's run |
| a row by its tuple key | `LocateByTupleKey` (`:9958`) | a lookup | the point arm, a list, a unique key's run (`WriteKeys`) |

Two rank facts the build follows. With a two-sided tuple key range and an
indexed equality both present, the read ladder takes the range (`id
BETWEEN 3 AND 6 AND owner = 1` is `Range`); with a one-sided bound it
takes the index (`id >= 3 AND owner = 1` is `IndexProbe`). A write's lock
window is wider than the read's range - it folds a one-sided bound - so
**the bounded walk takes the window's bounds, one-sided included, and the
index arm ranks above a one-sided bound and below a two-sided one**.

**What a write has no word for.** `ANALYZE UPDATE ...` is refused `expected
a SELECT statement`: `ANALYZE` is a dispatcher prefix over `HandleSelect`
(`:1991`). A test tells a write's descent from its walk by the leaves it
pins exclusive (`tests/key_set_test.cpp`, `KeySetWriteHoldTest`).
`indexes = off` is a read-path switch (`index.md` §12.3) the write's
existing unique-key arm does not ask.

**Another session's branch changes the same code.** `worktree-dt-write-path-yield`
at `d172085e8` (DT-S0..S3, not on `main`) edits `WalkBtreeLeaves`,
`WalkHeapPages`, `VisitRelation`, `WriteWalk` and the three statements'
walk sites. DU is built from `60dc593af` without it; whichever lands
second merges the two.

**The premise** `[measured]`
(`bench/v3.5.0/results-du-s0-premise-v3.4.17-150-g60dc593af.md`): at
1,000,000 rows, `cores = 1`, an `UPDATE` by an indexed non-unique key
costs 33.1-35.2 ms of server CPU walked, and 107 µs (10 rows a key) or
2,738 µs (1,000 rows a key) as an index read followed by a write by the
tuple keys it returned - 308x and 12.9x, against a control arm that moved
0.3 % and 4.3 %. **DU-Q1 (a)'s 5x bar is passed and DU-S2 is built.** The
host was not quiet (another session's build and suite ran through every
cell); the unit is on-CPU time and the verdict is a ratio 2.6 times its
bar at the least. **The same cells bound the gain**: the walk costs 33 ns
a row visited and 0.5-2.1 µs more a row written, the keyed form 2.7 µs a
row written at the margin, so somewhere between 1 row in 16 and 1 row in
65 of the relation the walk becomes the cheaper arm, which the arm taken
by rank alone does not see; DU-Q2's bound caps the cost and DU-Q4's pin is
the operator's lever. DU-S4 measures the built arm on both sides of that
crossing.

**Reviewed** (`critics-developer`, at `c2b138317`): the census's code
facts hold. Applied: DU-R2's conclusion and DU-R3's "unit for unit"
reworded to what the code does (a wait forgone, and the row a re-run
could write); the building-index premise and the collected-targets hazard
named; the units table's fourth-row wording; the results file's crossing
recomputed with the walk's own cost a row written, and its claim that the
on-CPU unit does not carry the 1,000-row cell's outlier withdrawn (it
does: W's mean is above its p75). Rejected: moving the two paragraphs on
`ANALYZE` and on the DT branch out of the census - both are facts a later
stage acts on, and the stage log is where a stage's facts are kept.

Overhead not measured; measured at the milestone's close.

### DU-S1 - the bounded walk (2026-11-26)

On `du-keyed-write`, from `835764c7a`.

**Built.** A write whose `WHERE` bounds its tuple key visits the window
and not the relation.

- **The bound** is read off the statement once (`WriteWalk::BoundBy`): the
  window `FoldTupleKeyWindow` folds - the one `DeclaredWriteBorrow`
  declares from, a one-sided bound included - met with the ids an embedded
  primary key's conjuncts name (`exec::WriteEmbeddedWindowOf`, the read
  ladder's `EmbeddedKeyWindowOnStep`). It bounds what is read and declares
  nothing: the unit a write declares is unchanged, and a window too narrow
  to declare (one key) or never declared (an embedded key's) still bounds.
- **A btree** (`WalkBtreeLeaves`, a `WalkBound`): the walk descends to the
  low key by the resume's own `BtreeSeekLeaf`, skips the keys below it,
  and ends at the first key at or past the high one - at most the leaf
  after the window's last is taken. A seek that fails on a first walk
  starts from the leftmost leaf instead of refusing (DU-R4).
- **A heap** has no key order to bound a walk by, so its window is a range
  of its Keystone index: the tuple keys are collected first
  (`CollectWriteKeys`, `exec::AppendIndexRangeTupleKeys`,
  `exec::KeystoneIndexBounds`) and written by the point arm once a key,
  ascending (`WriteKeys`), under the window the statement declared. Every
  window shape, where a read of a heap ranges only for `>=` with `<=`.
  `indexes = off` takes the range away and the heap walks.
- **`UPDATE`, `DELETE` and the locking read** take it through one copy
  (`WriteWalk::Visit`, `WriteByKeys`).
- **DU-Q2 (a) as built**: a statement's first collection is read to one
  entry past `max_locks_per_txn` and no further; past it nothing is
  written from it and the walk answers. The bound is on entries, not on
  distinct keys - an entry is at least a key, and counting entries is what
  lets the read stop. A resume's collection is read whole.
- **Not bounded**: a `WHERE` holding a subquery (DU-Q5 (a)), in its
  collection pass and its write walk alike.

**What changed for a statement that was answered before.** Nothing in its
rows or its units (below). Three things a client can see: a write over a
window whose rows are few no longer takes every leaf exclusive; a
malformed-chain check made at a walk's first row (`CheckConjuncts`,
BT-R5) is made at the first row the bounded walk visits, as a keyed read's
is; and on a heap a Keystone descent that fails after a row of the window
was offered is refused `TxnConflict` and run again (`DescentDeclined`),
where the walk would have gone on - the rule CU-S3's list on a heap
already has.

**Cells** (`tests/keyed_write_test.cpp`, new; `tests/leaf_watching_store.hpp`
moved out of `key_set_test.cpp`):

- the leaves held: a btree's window, two-sided, one-sided, strict, of one
  key, empty, past the relation, beside other conjuncts; an embedded key's
  prefix and range; a heap's declared and hidden Keystone; `indexes = off`;
  and what still walks every leaf (the bound where no arm reads it, a
  bound beside a subquery);
- DU-R1: 42 statements over a btree, a heap and an embedded-key relation,
  each run bounded in one instance and walked in a twin, the replies and
  every relation compared after each - deleted rows inside and at the edge
  of a window, retired slots (`PURGE`) with keys placed back among them, a
  `SET` that moves what the `WHERE` reads;
- DU-R3: the declared window and its mode read off the transaction's
  ledger, no row unit under it; an embedded key's bound declaring nothing
  and taking row units; an insert inside the window waiting for the decide
  and one outside not; a bounded walk parked on a held row resuming inside
  its bound, every row once;
- DU-Q2: a heap window past a cap of 6 answered by the walk, the Keystone
  index counted as used by neither.

Two cells of `key_set_test.cpp` changed with the behaviour: a bound on the
tuple key holds its window's leaves, not every leaf, and a single key's
declined descent falls to a walk bounded by that key.

**Reviewed** (`critics-developer`, the working tree over `c2b138317`): no
break found in DU-R1, DU-R3 or the `past` stop (retired slots carry no key
and go to the row function; delete-marked slots are keyed and in order).
Applied: the cap moved inside the range read, which before read a whole
window - a 10 M-row heap's whole Keystone index for `id > 0` - before
declining; a declined collection no longer counted as a use of the index;
`UniqueKeyRun` made a call of the one collector; the Keystone key's
encoding and the index lookup taken from `heap_store` instead of copied;
the bounds sized by the index layout; three redundant guards cut; the cap
cell and the retired-slot cells added. Not built: a cell for the heap
arm's resume - a declared window is waited for before the first row, so a
held row cannot park it mid-loop; the test file says so, and DU-S2's index
arm exercises the same `WriteKeys` resume.

The suite: 5,234 of 5,234 on the tree the stage commits. Overhead not measured;
measured at the milestone's close.

### DU-S2 - the index-driven write: an equality prefix and a list (2026-11-26)

On `du-keyed-write`, from `b248c1cb1`.

**Built.** An `UPDATE`, a `DELETE` or a locking read whose `WHERE` enters
an index finds its rows through it.

- **The arm is the read ladder's own choice** (`exec::WriteIndexArmOf`):
  `IndexProbeOf` over the write's compiled `WHERE` - the longest equality
  prefix, one value list standing as a prefix column, the lowest oid at a
  tie, never a building index - so a write and the `SELECT` of its `WHERE`
  enter the same index by the same bounds. A range after the prefix is
  declined here and is DU-S3's.
- **Collect, then write** (`CommandDispatcher::IndexArmKeys`,
  `CollectWriteKeys`): the tuple keys of the entries between the bounds - a
  list's, one pair a value - are read holding no page, made distinct and
  sorted, and each is written by the point arm (`WriteKeys`), where
  `Qualify` asks the whole `WHERE` of the row found and `Claim` takes the
  walk's unit. A resume collects again and starts at the key it stopped
  on.
- **The rank** (`WriteWalk::BoundBy`, `exec::WriteRangeOutranksIndex`) is
  the read ladder's: a list on the tuple key and a unique key named whole
  first, as before; then a btree's tuple key range with both ends, neither
  strict, and an embedded key's window, which are the bounded walk; then
  the index; then what a read does not rank - a one-sided or strict bound,
  and any bound on a heap - which bounds the walk, or reads a heap's
  Keystone range, only where no index is entered. A bound beside the index
  drops the collected keys outside it.
- **`indexes = off`** takes the arm away, as it takes a read's.
- **A resume of a walk is the walk's** (`WalkCursor::walked`): a statement
  that reached its walk - no arm, a declined descent, a collection past
  DU-Q2's bound - and parked there resumes there. Before, a btree's walk
  parked at a key and a resume asked the key arms again; that was sound,
  and with DU it would have read whole the collection the first run was
  refused for its size.

**DU-Q7 (a), adopted** (§4): on a heap the keyed arms write in key order
and the walk wrote in page order, so a statement whose rows are judged
against each other can change between an answer and a refusal. Found by
the review, pinned by a cell, stated for `index.md`.

**What changed for a statement that was answered before**, beyond DU-S1's
list: on a heap, DU-Q7; a write by an indexed column is a use of that
index in `sys.indexes` (`probes`, `entries_scanned`, `rows_resolved`,
`last_used`), where a write was a use of none; and the wait DU-S0's census
describes is forgone for a row whose matching version an undecided or
later writer put there after the collection passed it.

**Cells** (`tests/keyed_write_test.cpp`): the leaves held by an equality,
a list, a composite prefix, a miss, a bound beside the index, on a btree
and a heap, with `indexes = off`, and what still walks; the rank where it
is the read ladder's and where a strict bound or a heap changes it; DU-R1,
52 statements over a btree and a heap against a walked twin - entries left
by a key that moved, came back and was deleted, a covering index, a
truncated string key, a unique key's prefix, statements that move the key
they are found by; DU-R3, the row units of the rows written and of no row
the index named and the `WHERE` declined, off the ledger; a park on a held
row and the resume, a `v = v + 100` and a moved key showing each row once;
a newer committed version refused as the walk refuses it; DU-Q2 on an
index, and a walk taken for the cap resuming as a walk; a locking read by
an indexed column and through a view, each row claimed once in key order;
two cores; a foreign key in both directions, a unique key, an assertion
and a Transition asked of rows the index arm writes; DU-Q7's order.
**The index contract suite's write family**
(`tests/index_contract_test.cpp`, `EveryConfigurationIsLeftTheSameByEveryWrite`):
25 writes, each run in five configurations - write-hook-filled,
backfilled, `indexes = off`, no index, rebuilt - the reply and the whole
relation compared after each.

Three older cells changed with the behaviour: `primary_key_test.cpp` (half
a primary key and a non-unique index are now arms, one probe each) and
`index_usage_rig_test.cpp` (a write uses the index its `WHERE` enters and
no other).

**Reviewed** (`critics-developer`, the working tree over `b248c1cb1`): no
quiet-wrong found in the rows written - `CompileWhere` derives no
conjunct, a list's NULL and duplicate items and a refused value are the
read ladder's cases, the list is evaluated whole on the row, an entry's
tuple key is the Keystone on a heap, an `ON CONFLICT` arm and a subquery
reach no arm. Applied: the rank made the read ladder's where it had
ignored `strict` and a heap; `rows_resolved` counted after the bound drops
its keys; the walk's park marked (`WalkCursor::walked`); the comments on
the arms brought up to date; one helper for the three copies of a
collection's offer; the unreachable width checks cut; cells for the
locking read, the capped walk's resume, the rank on a heap, the checks and
DU-Q7. DU-Q7 itself is recorded as an accepted difference, not removed
(its options are in §4). Rejected: correcting the contract fixture's
comment on its string column - it predates DU and DU's long tags are in
`keyed_write_test.cpp`; the defensive `end()` test in `WriteIndexArmOf`
stays, since the alternative is a null dereference.

The suite: 5,249 of 5,250 on the tree the stage commits; the one failure
is `DurableKickRigTest.APeersGroupCommitIsKickedByTheWriter`, a flake of
this host measured at the base commit (5 of 30 alone at `60dc593af`),
which passed 13 of 15 alone on this tree, as it did on the tree before the
review's changes. Overhead not measured; measured at the milestone's
close.
