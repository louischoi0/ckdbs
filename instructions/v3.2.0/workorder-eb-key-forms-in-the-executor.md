# Work order EB - the key forms in the executor: a key range that is a range, a join by a whole key, an order the rows already hold, an upsert by any key

Status: **OPENED 2026-11-27** on worktree `eb-key-forms-in-the-executor`
(branch `worktree-eb-key-forms-in-the-executor`) from `9e7597d5d`
(`v3.4.17-224-g9e7597d5d`), on the operator's standing go-ahead of
2026-11-27 (go-ahead-achieving-milestone; `raft-marks-2026-11-27.md` §20).
EB-Q0..Q10 are CLA's proposals adopted on that go-ahead (§4 "Marks"),
none the operator's own per-item mark. Written 2026-11-26 on worktree
`known-gaps-milestone-orders` from `8274c951` (`v3.4.17-148-g8274c951e`)
on the operator's word of 2026-11-26 (`raft-marks-2026-11-26.md` §3). A
`[source-read]` claim with a `path:line` in §1 was read at `8274c951`; the
`known-gaps.md` lines it cites moved, and §7's "Opened" names where each
now stands at `9e7597d5d`.

Tags: `[operator]`, `[source-read]`, `[measured]`, `[design]`,
`[quiet-wrong]`, as in DT's order.

Related: `docs/spec/heap-and-tuple.md` §3.1b, §3.1c;
`docs/spec/parser-v2.md` I10-I12, I20; `docs/spec/index.md` §9, IX8c;
`docs/spec/plan-stats.md`; `docs/inflight/known-gaps.md`, "The heap as
data pages (DN)", "A primary key over the Keystone (DR)", "Ordered walks
(CV)"; `instructions/v3.2.0/workorder-dn-heap-rowid.md`;
`instructions/v3.2.0/workorder-dr-primary-key-over-keystone.md`
(DR-Q5, DR-Q17, DR-Q18); `instructions/v3.2.0/workorder-cv-ordered-index-walk.md`.

## 0. What EB is

`[source-read]` DN and DR gave the engine three key forms - the Keystone
itself, an embedded key, an indexed or hashed key - and a heap. Each is
answered correctly on every path, and on several paths the executor does
not yet read the form: a statement that names the key whole or in order
is run as if it named an ordinary column. `[design]` EB closes those
paths, one stage a family, each a cost and never an answer.

EB is **four families that share a subject and no code**; DN's and DR's
close each called several of them "its own order". EB-Q0 lets the
operator split it.

## 1. The tree as read

`[source-read]` from `docs/inflight/known-gaps.md` at `8274c951`:

**Ranges.**
- **A Keystone range is a full scan on a heap** (`:3499`): the Keystone
  index is not chosen for `WHERE keyidentity > 2`.
- **A page by key is a walk on every structure** (`:3641`, DR-Q17,
  `[measured]`): `WHERE key >= x ORDER BY key LIMIT n` is 1.0 ms at
  100,000 rows on a btree's tuple key and 5.8-9.0 ms on a heap's
  Keystone and on any index, "because a one-sided bound is a range to no
  arm of the ladder". "Not a primary key's gap, and its own order."
- **A one-sided bound does not seed the reverse walk** (`:3103`).

**Joins.**
- **A join key enters an index by its leading column only** (`:3657`,
  `CorrelatedIndexProbeOf`, `src/exec/step_compiler.cpp:1665`).
- **An embedded key's join is not keyed** (`:3701`): the id is computed
  from literals at compile, and nothing computes it from an outer row.
- **An embedded key's literal prefix on a joined relation is a range per
  outer row** (`:3716`).

**Order.**
- **`ORDER BY` an embedded key is sorted, not elided** (`:3706`).

**Upserts.**
- **A multi-column `ON CONFLICT` target is refused by the parser**
  (`:3661`, P2c, DR-Q5 (a)).
- **`ON CONFLICT` on a relation whose key is embedded is refused
  `NotImplemented` whatever its target** (`:3711`).
- **`ON CONFLICT (keyidentity)` on a hidden-Keystone heap is admitted,
  against `heap-and-tuple.md` §3.1b** (`:3513`;
  `ResolveConflictArbiter`, `src/server/command_dispatcher.cpp:8332`).
  "Whether the code refuses it or the spec admits it is not decided."

**Beside them.**
- **A heap has no sampled planner prior** (`:3504`), so a join block
  holding one is not captured under `join_order = fixed`.
- **The query layer refuses `keyidentity`** (`:3508`).
- **A hashed key's home serves one shape** (`:3670`): no `SELECT` inside
  a read-write transaction, because a view does not carry its level.
- **Partly filled heap pages are abandoned at each restart** (`:3519`);
  **`PURGE` on a heap walks the whole heap** (`:3528`).

**Not measured**: an embedded key's join, the literal-prefix range per
outer row, a write by a unique key above one core.

## 2. The mechanism `[design]`

1. **Ranges (S1).** A one-sided or two-sided bound on the tuple key, on
   a heap's Keystone or on an index's leading key after its equality
   prefix becomes a range arm of the ladder: a seek to the bound and an
   ordered walk from it, forward or reverse. Under `ORDER BY` the same
   key and a `LIMIT` it is CV's ordered walk with a seeded start.
2. **Joins (S2).** A correlated probe carries every join-key column
   that forms an index's equality prefix, not the first. For an embedded
   key, the step computes the id from the outer row's values by the
   code `CREATE TABLE` fixed, and descends by tuple key.
3. **Order (S3).** `ORDER BY` the columns of an embedded key, in key
   order, is elided as `ORDER BY <tuple key>` is.
4. **Upserts (S4).** The parser reads a column list as an `ON CONFLICT`
   target; the arbiter is the unique index whose key the list names
   whole, or the embedded key's computed id. The refusal on an embedded
   relation is narrowed to the targets that need what is not built.
   `:3513`'s spec conflict is the operator's (EB-Q4).
5. **Beside them (S5), as marked**: a heap's prior, the query layer's
   hidden column, the view's level.

## 3. Rules

- **EB-R1. Every arm EB adds returns the rows the arm it displaces
  returns** `[quiet-wrong]`, by the byte-for-byte contract suites: the
  index suite gains each new range and probe shape, and each key form
  runs it - the hashed form included, which has none today
  (`known-gaps.md:3682`).
- **EB-R2. Emission order stays no contract without `ORDER BY`**, and an
  elided sort returns ties as the sort it replaces does, or is not
  elided.
- **EB-R3. An embedded id computed from an outer row is the id
  `CREATE TABLE`'s code gives for those values** `[quiet-wrong]`: one
  encoder, called from both places, and a value outside a column's range
  is no match rather than another row's id.
- **EB-R4. A captured or administrator's plan keeps meaning what it
  meant**: a new arm is a new member of the ladder the plan's `ACCESS`
  restricts, and a plan that pinned `SCAN` still scans.
- **EB-R5. No form is added and no persisted format moves**; the key
  form stays fixed at `CREATE TABLE`.
- **EB-R6. A shape not served is answered as today.**
- The standing rules, as DT's order lists them.

## 4. Items for the operator (EB-Q), CLA's proposals

| | item | options | CLA proposes |
|---|---|---|---|
| EB-Q0 | One order or several | (a) one order, a family a stage, each landing whole; (b) four orders; (c) ranges and joins only | (a) |
| EB-Q1 | Does EB-S0 gate each family? | (a) S0 measures each family's cost today against its hand-written best; a family under 2x is recorded and left. (b) all built | (a) |
| EB-Q2 | The range arm's rank in the ladder | (a) below an equality probe, above a walk, priced by the planner's statistics where a prior exists; (b) by rank only | (a) |
| EB-Q3 | `kFingerprintVersion` | (a) does not move: a multi-column `ON CONFLICT` target is a shape that was refused, and S0 confirms the bump rule reads it so; (b) bumped if the rule requires | (a), S0 to confirm |
| EB-Q4 | `ON CONFLICT (keyidentity)` on a hidden-Keystone heap | (a) the code refuses it, as the spec says; (b) the spec admits it, as the code does | (a): truthfulness to the spec as written, and the statement is refused by name |
| EB-Q5 | A heap's sampled prior | (a) inside S5: a sample of data pages, so a heap's block can be captured; (b) outside - `GATHER STATISTICS` is the answer | (a) |
| EB-Q6 | A view that carries its level (the hashed home inside a read-write transaction) | (a) outside EB: a snapshot's layout is the transaction manager's; (b) inside | (a) |
| EB-Q7 | The heap's restart hints and `PURGE` by the Keystone index | (a) outside EB; (b) inside S5 | (a) |
| EB-Q8 | A descending prefix of an embedded key (added at the opening, from the review) | (a) stays sorted: its ties would leave by descending tuple key where the sort's leave ascending (EB-R2); an ascending prefix, and the whole key either way, are elided; (b) elided, ties descending, as CV-Q4 (a) chose for an index walk | (a) |
| EB-Q9 | The write ladder (added at the opening) | (a) EB moves the read ladder only: `WriteIndexArmOf` and `WriteRangeOutranksIndex` read the bounds as today, as `tests/keyed_write_test.cpp`'s "A one-sided range on the column is none to the ladder" and "A strict range ... rank below the index" pin, and §6 says the write side is DU's; (b) both ladders | (a) |
| EB-Q10 | Waystone and a computed-key probe (added at the opening) | (a) a probe whose key is computed from an outer row's columns records no trail and replays none, so invariants 8 and 9 are not reached; (b) the trail contract extended to it | (a) |

### Marks

All adopted on the operator's standing go-ahead of 2026-11-27
(go-ahead-achieving-milestone), never the operator's own per-item mark
(`raft-marks-2026-11-27.md` §20):

- **EB-Q0 (a)**, **EB-Q1 (a)**, **EB-Q3 (a)** - confirmed by reading
  `include/kds/parser/fingerprint.hpp`'s bump rule: making a refused
  statement admitted moves no stored hash - **EB-Q4 (a)**, **EB-Q6 (a)**,
  **EB-Q7 (a)**, **EB-Q8 (a)**, **EB-Q9 (a)**, **EB-Q10 (a)**.
- **EB-Q2 revised, (a) to (b), and (b) adopted**: (a) priced the arm by the
  planner's statistics, which `docs/spec/index.md` IX9 forbids (an arm is
  `f(shape, catalog)`), and placed a range above other arms, which reopens
  the operator's ruling (a) of 2026-10-15 at `TupleKeyRangeOf` ("a strict
  range ranks last ... where a range belongs against those arms is
  undecided"). As adopted: **a one-sided or strict end ranks where a
  strict tuple key range ranks - it replaces a bare walk and nothing else -
  and an index or a heap's Keystone index takes one only where its walk in
  key order seeds an elided `ORDER BY ... LIMIT`** (CV's ordered walk), the
  one place a range with an open end is known to stop early.
- **EB-Q5 revised, (a) to (b), and (b) adopted**: a heap's sampled prior is
  the statistics steward's, shares no code with the key forms, and
  `GATHER STATISTICS` already counts a heap exactly. S5 holds no item.

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **EB-S0** | The premise per family, measured in `build-release` at 100,000 and 1,000,000 rows on each key form and a heap; the ladder's ranks as read; the bump rule read against S4 | a results file; EB-Q1's bar per family |
| **EB-S1** | Ranges | the contract suite's range family on every form; `ANALYZE` names the arm; CV's cells with a seeded start |
| **EB-S2** | Joins by a whole key | the contract suite's probe family; EB-R3's cells at each column type's bounds |
| **EB-S3** | The elided order | cells: ties; `DESC`; a partial key prefix is still sorted |
| **EB-S4** | Upserts | cells per arbiter; the differential against PostgreSQL 18 (`scripts/bs/difftest.py`) for the shapes both accept |
| **EB-S5** | What EB-Q5..Q7 mark | per item |
| **EB-S6** | The close: overhead A/B on a point read and a point write of each form, the specs, the manual, `known-gaps.md` | the operator's word on landing |

## 6. What EB does not do

- No index-only scan, no new key form, no change of a relation's form
  after `CREATE TABLE`.
- No row moved to its home (K1: an id is not rebound).
- No column statistics (the operator's W13).
- No write-side arm: DU's.

## 7. Stage log (CLA, appended as stages land)

### Opened 2026-11-27

On `eb-key-forms-in-the-executor` from `9e7597d5d`, on the operator's
standing go-ahead (`raft-marks-2026-11-27.md` §20).

**§1 read again at `9e7597d5d`.** Every item stands but one: **a two-sided
inclusive range on a heap's Keystone is already an index range** - the
Keystone index is one of `TableAccess::indexes` and `IndexProbeOf` does
not skip it (`src/catalog/catalog.cpp:3023`, `docs/spec/index.md` §9a:
"a read ranges a heap's Keystone only for `>=` with `<=`"); what is a walk
is a strict end, a one-sided bound, and `ORDER BY keyidentity` under a
`LIMIT` (`DriverOrderOf` drops the tuple key, the elision skips a heap).
`known-gaps.md`'s lines, the order's then now: 3499 to 3927, 3641 to 4167,
3103 to 3534, 3657 to 4185, 3701 to 4229, 3716 to 4243, 3706 to 4234,
3661 to 4189, 3711 to 4238, 3513 to 3940, 3504 to 3932, 3508 to 3936, 3670
to 4198, 3519 to 3947, 3528 to 3956, 3682 to 4210. `ResolveConflictArbiter`
is at `src/server/command_dispatcher.cpp:8537`; the parser's target refusal
at `src/parser/parser.cpp:2742`.

**The review the writing left unfinished** (`critics-developer`, on the
order against `9e7597d5d`, reading only). Applied: S1 moves the write
ladder unless kept out (EB-Q9 added); the range kinds, the open ends'
padding (`ProbeOverIndex`) and the reverse walk's seed from a `kRange`
already exist, so S1 widens what reads bounds and adds no kind; the heap's
two-sided Keystone range already served (above); EB-Q2 (a) against IX9 and
ruling (a) (revised); a computed-key probe is `kProbe`-like and Waystone
replays `kProbe` (EB-Q10 added); the one encoder is `exec::EmbeddedKeyId`
(`include/kds/exec/embedded_key.hpp`), which EB-R3 names; S2 takes a
prefix mixing literal and correlated equalities, the common case; EB-Q3
needs no operator item (read and recorded); an ascending prefix of an
embedded key can be elided (EB-Q8 added for the descending one); S5's
items share no code with the key forms (EB-Q5 revised); the missing cells
- `v >= NULL`, a bound above `2^48 - 1`, a strict end on a non-integer
index column, a plan's `ACCESS` variant a new arm now meets - go into the
stages that add each shape. **Rejected**: "measure only joins and order,
at 100,000 rows" - the ranges' shapes S1 serves are not DR-Q17's (strict
windows, a heap's page), and all of S0 fits its 20 minutes; "fold S4 into
another stage" - a stage that lands whole costs nothing more for being
small; EB-Q4's widening to every hidden Keystone - S4 reads the spec for a
btree's hidden Keystone before it refuses anything the spec does not say.

### EB-S0 - the premise, 2026-11-27

Measured at `b761306e0` (`v3.4.17-225-gb761306e0`), whose build inputs are
`9e7597d5d`'s. Results: `bench/v3.5.0/results-eb-s0-premise-v3.4.17-225-gb761306e0.md`.
Each shape was run against a proxy, the cheapest statement or schema
returning the same rows through an arm that exists:

- **S1** (ranges) passes EB-Q1 (a)'s bar: 13.5x to 50.4x at 100,000 rows
  and 119x to 391x at 1,000,000.
- **S2** (joins) passes on its two whole-key cells: a composite index key
  at 1,286x and 20,422x; an embedded key at 4,554x, and **refused** by the
  row-touch budget at 1,000,000 rows.
- **S3** (order) passes: 263x and 2,546x.
- **The literal-prefix join is 2.77x and, as measured, 1.54x**: the
  planner already reorders it to one range and a build. The S0 review
  found the 1,000,000-row cell carrying about 95 us added to every arm
  alike (bimodal latencies, a T - B gap unchanged at 81 us and 78 us), so
  the estimate is about 2.6x. **It is built with S2**: its key is the
  embedded key pinned whole, a literal and an outer column, and S2's
  computed id outranks the literal prefix's window there.
- **S4** is a refusal family, which the bar cannot price. CLA reads it as
  built: what an operator writes instead is a transaction that reads and
  then writes.

These readings are CLA's, on the go-ahead. **The S0 review**
(`critics-developer`, at `1192c47ce`) corrected the results file in place:
J1's entry counts (1,000,000 and 10,000,000 resolved a statement, linear
in `n` with the outer side fixed), the T2 bound (1.31 %), the budget's
cuts at 100,000 rows, a refusal quoted short, units in the ratio cells,
and the run-time `df -T` and per-arm `pgrep` the driver did not record.
Its J3 finding is applied above. **Rejected**: "S4 read as built is the
operator's call" - the go-ahead makes CLA's proposal the mark, and the
reading is recorded as CLA's; a re-run of J3 on a quiet host - S2 builds
it either way.

### EB-S1 - ranges, 2026-11-27

On `eb-key-forms-in-the-executor` over `e2dec50f7`. **A bound with an open
or a strict end is served, at the rank EB-Q2 (b) as adopted gives it**
(`docs/spec/index.md` IX8 and IX9's "open or strict end" item):

- **a btree's tuple key bounded at one end** is a range to the key space's
  end (`TupleKeyRangeOf`'s `open_ends`, `RangeBounds::open`), taken on a
  bare walk only and last; it seeds the reverse walk, so keyset pagination
  (`id < x ORDER BY id DESC LIMIT n`) reads its page;
- **a window with a strict end on an index**, a heap's Keystone index
  included, is the index's range on a bare walk (`BoundEnds::kStrict`), each
  strict end read inclusively and the residual dropping its equal entries;
- **an open end seeds CV's ordered walk** (`BoundEnds::kOpen`,
  `OnlyBoundsOn`): with no prefix where every conjunct bounds the first
  order key, after a prefix on the next key; and **a heap's `ORDER BY
  keyidentity` walks its Keystone index** (`DriverOrderOf`).

The write ladder reads the bounds it always read (EB-Q9 (a)); DU's tests
pass unchanged. Two older cells pinned the behaviour S1 replaces and were
rewritten to the new contract: `StepCompileTest.AStrictBoundThatAdmitsNoKeyIsNotARangeAndOneEndAloneIsAnOpenOne`
and `IndexUsageRig.ARangeIsCountedApartFromAProbe` (a strict window now
counts as a range). The index contract suite carries every new shape
(EB-R1), and `tests/ordered_walk_test.cpp`'s `KeyBoundTest` the arms, the
ranks, the heap and the string key past its stored prefix.

**The review** (`critics-developer`, on the change over `1192c47ce`) found
no correctness defect and read every `[quiet-wrong]` hazard put to it as
safe - a strict end on a truncated string, the open end's padding against
the `0x01` discriminator, a NULL or unencodable bound, the Cabin set's
gate, the heap's elision, the pins, the embedded window, Waystone.
**Applied**: an open range was priced by the join planner as a window as
wide as the key space, so `t.id >= x` in a join block weighed the whole
relation where a bound weighs `kRangeEndSelectivity` - `RangeBounds::open`
now prices it as a bound (`join_order.cpp`); an open end could move a step
the ladder put on another index's prefix-less range to the ordered walk,
which that index's narrower range must keep - the ordered walk reads only
inclusive bounds on any other index there; three cuts (`BoundsOnColumn`'s
strictness flags, the last ranks as one chain, `OnlyBoundsOn` taking an
empty residual); the `known-gaps.md` wording; four cells (a string past its
prefix, a `TUPLE_KEY` pin on `id >= x` that now fits - a variant declined
before is now met, EB-R4 - a heap's key set keeping its sort, a join with
an open bound). **Rejected**: none.

Overhead not measured; measured at the milestone's close.

### EB-S2 - joins by a whole key, 2026-11-27

On `eb-key-forms-in-the-executor` over `cf3f5bb87`.

- **A correlated index probe takes every prefix column an equality pins**
  (`docs/spec/index.md` IX17): a literal's, encoded at compile, or an outer
  row's, encoded per outer row at its offset (`IndexProbe::key_from`, now
  one entry a column). Of the indexes the longest prefix, ties to the
  lowest oid; a literal probe yields to a correlated one only where the
  correlated prefix is longer. EB-S0's J1 - a join on both columns of
  `(a, b)` - descends by the whole key.
- **An embedded primary key pinned whole by literals and an outer row is a
  `kProbe` by the computed id** (`heap-and-tuple.md` §3.1c,
  `Step::EmbeddedProbe`): the executor calls `exec::EmbeddedKeyId`, the
  encoder every insert placed its row by (EB-R3); a NULL, a value past its
  column's width and the one key 48 bits cannot name match nothing; any
  other refusal is `Corruption`, never an empty answer. It ranks with the
  key's window and above it, which serves EB-S0's J2 and J3. It records
  and replays no trail (EB-Q10 (a); `IsTrailReplayable`).

The cells: `tests/ordered_walk_test.cpp`'s `WholeKeyJoinTest` against a twin
with no index or key, each column's type bounds, NULLs on either side, the
2^48 key, a literal past its column, and `EmbeddedProbeTrailTest`; the
index contract suite's whole-key join block (EB-R1).

**The review** (`critics-developer`, report only) found no `[quiet-wrong]`
defect: the `key_from` order, the templates, a NULL outer value, the stale
chain's guard, every reader of the probe (Waystone, the pins, the ordered
walk, the write paths, the hashed home), the residual's coverage of every
operand, and the literals' coercion each read as sound. **Applied**: F1 -
every `EmbeddedKeyId` refusal was read as "no match", where only a NULL and
`OutOfRange` are known to match nothing (CU-Q11's rule); F2 - the join
planner priced an embedded key bound whole as many rows, now at most one,
as a unique index bound whole is; S1 - one source search,
`EqualitySourceOn`, for both arms; S2 - a second coercion of a literal the
lowering already coerced, removed. **Rejected**: S3 (the correlated search
run twice at compile) - a compile-only cost, and returning the probe from
the literal arm would widen that arm's contract for it; F3 (an embedded
probe filed under column 0 in the access statistics) - a literal embedded
lookup is already filed so, and the filing is the tuple key's.

The full suite after the change, before the review's edits: 5,394 of 5,395,
the one failure `SetExprTwoCoreRigTest.TwoCoresIncrementingOneRowLoseNoIncrement`,
which failed 11 of 60 repeated alone on this tree - the rate memory records
for it at base, on a two-core path S2 does not touch.

Overhead not measured; measured at the milestone's close.

### EB-S3 - the elided order, 2026-11-27

On `eb-key-forms-in-the-executor` over `78f98354b`. **An `ORDER BY` an
embedded primary key already holds is not sorted** (`parser-v2.md` I11,
`EmbeddedKeyOrderOf`): the key's columns in key order all ascending, whole
or a prefix, wherever the driving step emits tuple key order, and the whole
key all descending under a `LIMIT` by the reverse walk. A descending prefix,
mixed directions, the columns out of key order and a column past the key
keep their sort (EB-Q8 (a)). A bare walk is not moved to a user index on the
key's columns for that order: the walk holds it for nothing. The cells:
`tests/ordered_walk_test.cpp`'s `EmbeddedOrderTest`, each against a twin
whose tuple keys were issued in key order, so its sort's ties are the key's
order too - every embeddable type (`int8` with negatives, `bool`, `date`,
`int32`, `int16`), a join the embedded relation drives, a user index.

**The review** (`critics-developer`, report only) found no `[quiet-wrong]`
defect: the code is order-preserving for every embeddable type, the sort
breaks ties by arrival so an ascending prefix's ties match, slot 0 is
`steps[0]` after a reorder, and OFFSET, views, the query layer, folds,
`FOR UPDATE` and `ALTER TABLE` cannot reach a wrong order. **Applied**: an
ordered index walk took a bare walk's free order over (now declined in
`OrderedIndexChoiceOf`); the four booleans folded into one direction and
`EmbeddedKeyOrderOf`, and two dead checks removed; the `known-gaps.md`
wording; three cells. **Rejected**: eliding the whole key followed by more
columns (`ORDER BY a, b, v`) - outside S3's cells, which state that a
column past the key keeps its sort, and a unique key's vacuous tail is a
rule of its own to write.

The full suite before the review's edits: 5,398 of 5,399, the one failure
`DurableKickRigTest.APeersGroupCommitIsKickedByTheWriter`, which failed 4 of
30 repeated alone on that binary - the rate memory records for it at base,
on the group commit's kick, which S3 does not touch. The suite after them
ran with S4's code in the tree (S4's stage log).

Overhead not measured; measured at the milestone's close.

### EB-S4 - upserts by any key, 2026-11-27

On `eb-key-forms-in-the-executor` over `88164d7fb`. **A conflict target is
a column list** (`parser-v2.md` I20): the arbiter is the unique index whose
key the columns are whole, in any order (`ResolveConflictArbiter`); **on a
relation whose primary key is embedded the key is the tuple key**, no target
or its columns in any order taking the tuple key arm at the id the row's key
encodes to (`ConflictTupleKeyOf`, the insert's own `EmbeddedRowId`), and
another unique index's key arbitrating there too (`heap-and-tuple.md`
§3.1c); **`ON CONFLICT (keyidentity)` on a hidden-Keystone heap is refused**
(EB-Q4 (a), BZ-R4). `kFingerprintVersion` did not move (EB-Q3 (a)): the
fingerprint is the token stream's, and a statement the parser refused was
never storable.

**The differential against PostgreSQL 18** (`tools/eb_s4_difftest.py`, a
throwaway `postgres:18` container, PostgreSQL 18.7): 13 statements over a
unique index of two columns and a primary key of two narrow integers -
`DO NOTHING` and `DO UPDATE` by the key written in either order, no target,
the key's bounds, two rows naming one key under `DO UPDATE`, a target of
one key column - and **no mismatch**: each statement ok or refused on both,
and the rows equal after each.

**The review** (`critics-developer`, report only, with probes against the
built server). **Applied**: **a `[quiet-wrong]` defect** - on an embedded
relation `ConflictTupleKeyOf` dropped the `keyidentity` a row wrote, so a
row naming another id than its key's, which the insert refuses, was
skipped under `DO NOTHING` and updated the key's row under `DO UPDATE`; it
now reads the written id under `EmbeddedRowId`'s own rule and the row is
refused, the cell in `btree_primary_key_test.cpp`; a refusal's byte pointed
at the row's first value where the key's column stands elsewhere, now the
key's (`ConflictKey::byte`, cell at byte 49); the `parser-v2.md` and
`ast.hpp` wording; three cuts (one embedded test, the arbiter's refusal
built once, `ta.oid` for a parameter). **Recorded, not EB's**: `DO NOTHING`
on the tuple key turns a held key's row refusal into a skip
(`known-gaps.md`). **Rejected**: none.

Overhead not measured; measured at the milestone's close.

### EB-S5 - nothing marked inside, 2026-11-27

EB-Q5 (b), EB-Q6 (a) and EB-Q7 (a) put a heap's sampled prior, a view that
carries its level and the heap's restart hints with `PURGE` by the
Keystone index outside EB; no item is left for S5, and no code is written
for it. The query layer's refusal of `keyidentity` (`known-gaps.md`) was
listed beside them in §1 with no item, and stays.

### Written 2026-11-26

On `known-gaps-milestone-orders` from `8274c951`, on the operator's word.
No stage is opened and no code is written. **Not reviewed**: the
`critics-developer` review was started and stopped before it reported,
so no line reference or reading in this order has a second reader behind
it.
