# Work order CY - managed statistics: persisted, per column, per column group, and operated by a DBA or an agent

**Written 2026-11-18, not opened**, on worktree `cx-plan-change-option`
from `d4bda61f` (`v3.4.12-16-gd4bda61f`), on the operator's word, recorded
in English (`raft-marks-2026-11-18.md` §5):

- **W7**: *"From the list below, leaving item 4 out for now, bring the
  statistics features into the milestone or create a new one. Item 4 needs
  more thought."* - the list being CLA's five gaps in the engine's
  statistics against commercial systems: (1) no column statistics, (2) no
  persistence, (3) no multi-column correlation or functional dependency,
  (4) no rule for when to gather again, (5) no management surface.
- **W8** (2026-11-18, `raft-marks-2026-11-18.md` §6): *"CY-Q0, CY-Q1 $!a
  push no verify"* - CY-S1 is CX-S0's progress gate, and S2 and S3 are
  built only if S0 meets CC's flip gate.

**A new order, not a stage of CX**: CLA's choice, which W7 left open. CX-S4
held items 1 and 5 in eleven lines; with items 2 and 3 the work has a
catalog relation and a superblock bump of its own, a collection path, an
estimator and its own measurement, and one of its stages gates CX-S0
(CY-Q0). CX-S4 now points here.

**Item 4 is not in CY.** It is recorded as the operator's deferred question
in `docs/pending/statistics-staleness-and-regather.md`. Until it is ruled
on, **nothing in CY gathers again on its own** (CY-R3).

**Claim tags**: `[source-read]` with `path:line` at `d4bda61f`; `[design]`
for reasoning about code not yet written; `[measured]` with its results
file; `[quiet-wrong]` where a wrong choice turns a correct answer into a
wrong one with no error; `[operator]` for W7.

**The letter.** `[source-read]` CX is the last work order under
`instructions/v3.2.0/` at `d4bda61f`, and no file, branch or commit subject
fetched names a `CY-` token. CLA's choice.

**CY against other orders and specs.** CY extends CC
(`workorder-cc-plan-statistics.md`, CLOSED): CC's three facts and their
steward stay as built, and CY adds a persisted model beside them. It amends
`docs/spec/plan-stats.md` §1 ("memory-resident ... nothing is logged, no
catalog relation or page holds it") for the persisted part only. It is read
by CX (`workorder-cx-managed-plans.md`) through CY-R1 and changes none of
CX's rules. It is not part of AR0 §8's chain.

---

## 0. What CY is

The join planner prices an order from three in-memory facts and four
constants. CY gives it a statistics model that **survives a restart**
(item 2), knows **each column's distribution** (item 1) and **declared
column groups** (item 3), and is **an object a DBA or an agent operates**
(item 5): gathered, locked, exported, imported, versioned and restored by
explicit acts, so that a plan decision made on one instance can be
reproduced on another or in a test.

A statistic stays what `plan-stats.md` §1 makes it: advisory, and only to
the order. It chooses which of a block's legal orders runs and which side
is built; it never changes a row a statement returns.

### 0.1 Not in CY

- **When to gather again** (item 4): no staleness test, no modification
  counter read, no automatic re-gather
  (`docs/pending/statistics-staleness-and-regather.md`).
- Automatic discovery of correlated columns. A column group is declared
  (CY-S3).
- Statistics on an expression or a function conjunct (`DATE(ts) = ...`);
  it keeps its constant (CY-Q13).
- Cardinality feedback beyond CC's `card(T)` posteriors, and any change
  to them.
- An index's physical correlation (PostgreSQL's `correlation`). A
  secondary index entry leads to a tuple key descent whatever the order of
  the heap (`index.md`), so the planner prices no sequential-versus-random
  difference that it would feed.

## 1. The tree as read at `d4bda61f`

### 1.1 Selectivity is three constants

- `[source-read]` `kEqualitySelectivity = 0.01`, `kRangeEndSelectivity =
  0.3` (a two-sided range keeps the square) and `kOtherSelectivity = 0.5`
  (`include/kds/exec/join_order.hpp:42-44`), applied per conjunct in
  `src/exec/join_order.cpp:124-182`. The header says of them: "the model
  where no statistic is published, not measurements" (`:23-25`).
- `[source-read]` The only column-level knowledge is an identifier bound:
  a unique index whose every key column is equality-bound holds one row,
  and a Transition's state count or a foreign key's parent prior may only
  **raise** an equality's selectivity to `1/bound`, never lower it
  (`workorder-cc-plan-statistics.md` CC-Q11 (a)), because an upper bound on
  the distinct values bounds the average selectivity from below only.
- `[source-read]` "The engine keeps no row count, NDV or histogram today"
  (`workorder-cc-plan-statistics.md:74`), and that is still so.

### 1.2 What the planner does know is in memory only

- `[source-read]` `stats::PlanStats` holds a relation prior (PS1), a
  join-subset posterior `card(T)` (PS2) and a decline fact (PS3). It is
  "memory-resident: a restart or a restore empties it, nothing is logged,
  no catalog relation or page holds it, and the planner prices data-free
  until its first publish" (`docs/spec/plan-stats.md` §1).
- `[source-read]` Until a prior is published a relation is priced at
  `kAssumedRelationRows = 100000` (`join_order.hpp:33`). The steward steps
  once a second and seeds within 16 page reads a step, so a block's priors
  "can take a few steps to arrive" (`plan-stats.md` §4).
- `[source-read]` `docs/inflight/known-gaps.md`, "The join planner (CB)":
  a subset no run measured is priced from priors and the constants, "which
  can be wrong by orders of magnitude for a skewed join"; the prior samples
  at most four root children.
- `[design]` **Under CX's `fixed` default this stops being a warm-up
  cost.** A shape first compiled before the first publish is fixed on the
  data-free order (`workorder-cx-managed-plans.md` CX-S0, CX-Q19).

### 1.3 Nothing collects, and nothing is managed

- `[source-read]` Collection is a by-product of a planned statement's run
  and of the steward's seeds. "INSERT, UPDATE, DELETE and the KWP load:
  nothing. There are no commit-time deltas, no per-row counters and no
  change on the write path" (`workorder-cc-plan-statistics.md:280`).
- `[source-read]` No statement gathers, locks, exports or imports a
  statistic; `sys.relation_stats` shows `est_rows` and `generation`, which
  the planner never reads back from the view (`plan-stats.md` §6).

### 1.4 What CY builds on

- `[source-read]` A logged declaration relation with a fixed root page and
  a superblock bump is the tree's pattern for a new catalog object:
  `sys.transitions` (superblock 24), `sys.cabin_exprs` (26). The superblock
  is 35 (`include/kds/server/superblock.hpp:229`).
- `[source-read]` A `SELECT`'s outermost walk yields every 64 pages, 16 for
  an `analytic` session (`docs/spec/sched.md` §3), holding its view and its
  relation borrow.
- `[source-read]` A column keeps its oid for life; an `ADD COLUMN` that
  reuses a cleaned dropped slot keeps the oid and bumps `generation`
  (`docs/spec/alter.md` AL11).
- `[source-read]` A restore replays the log from the backup's start, so
  logged catalog state comes back and unlogged state is discarded
  (`docs/spec/backup.md` B6a).

## 2. Rules

- **CY-R1 - one reader** `[design]`. The persisted model is read by
  `exec::EstimateChain` and by `ANALYZE`'s estimates, as CC's facts are
  (`plan-stats.md` PS9), and so by exactly the compiles CX-R1 lets search:
  `planned`'s every compile, a `fixed` first compile, `ADVISE PLAN`. No
  access-ladder rung, legality test or executor path reads it.
- **CY-R2 - a statistic never changes a result** `[quiet-wrong]`. No
  estimate declares a result empty, skips a step or shortens a walk. Every
  rows value is clamped at 1 (PS9). A histogram that says "no row in this
  range" prices one row.
- **CY-R3 - the persisted model changes only by an explicit act**
  `[operator]`. `GATHER`, `IMPORT`, `RESTORE STATISTICS` and `PUBLISH`
  change it, and nothing else does, until item 4 is ruled on. The one
  exception CLA proposes is CY-Q2: the steward saving the relation priors
  it already computes, so that a restart does not start blind.
- **CY-R4 - a missing statistic is today's constant** `[design]`. Where no
  statistic exists for a conjunct, the constant of §1.1 prices it, byte for
  byte as today, so an instance that never gathers plans exactly as CC
  built. CC's direction rule stands where only a bound is known.
- **CY-R5 - the costly direction keeps its margin** `[design]`. An
  estimate too low chooses a build the executor then declines into a
  per-row walk; CC's quarter-cap guard (`kBuildCapMargin`, CC-Q9) applies
  to an estimate from any source.
- **CY-R6 - every stored statistic has a version** `[design]`. A
  statistics object (a relation's, a column's, a group's) carries a
  version that only grows and is logged with it. A plan decision can name
  the versions it read (CX-Q25), history can restore one (CY-S4), and an
  export names what it exported.
- **CY-R7 - identity is by oid and generation, never by name**
  `[quiet-wrong]`. A column statistic is keyed by `(relation oid, column
  oid, column generation)`, so a column added into a reused slot (§1.4)
  misses the dropped column's statistic instead of inheriting it.
- **CY-R8 - gathering is a reader** `[design]`. `GATHER` walks as an
  `analytic` read does - sliced, yielding, under the relation's `IS`
  borrow - takes no `X`, blocks no writer, and writes its result in one
  short transaction of its own at the end. It adds nothing to `INSERT`,
  `UPDATE` or `DELETE`.
- **CY-R9 - published values are banded** `[design]`. What the planner
  reads of a statistic is its half-octave band with CC's hysteresis
  (`plan-stats.md` PS5), so a re-gather that moves a value by a few
  percent publishes nothing and moves no plan. The stored value is exact;
  the band is the planner's view of it.

## 3. Stages

A stage opens on the operator's word. S0 decides whether S2 and S3 are
built at all; S1 does not wait for it (CY-Q1).

### CY-S0 - the premise and the census (no behaviour change)

`build-release`, inside 20 minutes, results at
`bench/v3.4.12/results-cy-s0-premise-<describe>.md`.

- **How wrong the constants are**: for every planned statement of
  scenarios 1-4, each step's `est=` against `obs=` from `ANALYZE`
  (`plan-stats.md` §6), with the true selectivity of each literal conjunct
  computed by a `COUNT`.
- **Which plans it costs**: the same statements re-planned with the true
  selectivities injected through the planner's test seam, listing every
  block whose order or build side changes and its p50 under both orders.
- **What a blind first compile costs**: each shape's order from an empty
  snapshot against its order once priors are published - the population
  CX-Q19 is about.
- **The census**: every reader of the three constants, every place a
  column's type meets a comparison in the cost model, and what a gather
  walk can reuse of the scan kernel (BM) for decoding cells.

### CY-S1 - persistence (item 2)

- **`sys.statistics`** `[design]`: a logged catalog relation (superblock
  36), one row per statistics object, holding its key (CY-R7), kind
  (relation, column, column group), origin (`gathered`, `observed`,
  `imported`), version (CY-R6), state (`published`, `pending`, `locked`
  as a flag), the time it was made, the rows sampled, and its values as
  one spilled cell.
- **The relation object**: rows and leaves. `GATHER STATISTICS ON t`
  with no column list writes it exactly, from a whole walk.
- **The mount seeds the snapshot before the listener opens** from the
  published relation objects, so the first compile after a restart or a
  restore prices from them, not from `kAssumedRelationRows`. The steward
  then runs as built; a live prior and a persisted one for the same
  relation is CY-Q3.
- **The steward's own priors saved** (CY-Q2): when a relation's prior band
  moves, the steward stages an `observed` relation object, written off the
  statement by a `system` task. A locked object is not overwritten.
- **A restore** brings back the model the backup's log held. It describes
  the restored data as of its last write, which is the property a backup
  has; `ANALYZE` shows each object's age.
- **Cells**: a restart and a restore each plan a scenario join at the
  first statement as the previous run did; a `READ ONLY` session's planned
  statement appends no record; a volume at superblock 35 is refused.

### CY-S2 - column statistics (item 1)

- **`GATHER STATISTICS ON t (c, ...)`**, or `ALL COLUMNS`, `[SAMPLE n
  PERCENT]` (CY-Q4). Per column: the NULL fraction, the number of distinct
  values, a most-common-values list with frequencies, and an equi-depth
  histogram over the values the list does not hold (CY-Q5). A
  `char`/`varchar` column is CY-Q6.
- **The estimator** `[design]`, replacing a constant only where a column
  object is published (CY-R4):
  - `c = v`: `v`'s frequency if the list holds it; otherwise the non-NULL,
    non-listed fraction over the remaining distinct values;
  - a range end or `BETWEEN`: the histogram's fraction, interpolated
    inside a bucket for numeric, date and timestamp types, plus the listed
    values inside the range;
  - `IS NULL` / `IS NOT NULL`: the NULL fraction and its complement;
  - `c <> v`, `IN (...)`, `NOT IN (...)`: from the same parts;
  - a join-key equality with no measured `card(T)`: the product of both
    sides' rows over the larger distinct count, less each side's NULLs -
    the standard containment estimate - in place of `1 / rows`.
  A measured `card(T)` still outranks every estimate (PS2).
- **A value arriving as `?`** is a literal by the compile (`C_BIND`
  substitutes, `kwp_session.cpp:580-585`), so the estimator always sees
  the value.
- **`ANALYZE`** names, per step, the source of each estimate: `const`,
  `bound`, `stat` or `obs`.
- **Cells**: the differential matrix - for every storable type and each
  comparison, the estimate against a `COUNT` on uniform, skewed (Zipf) and
  heavily-NULL data, within a stated factor; the four contract suites
  (waystone, index, cabin, inner-build) run under an empty model and under
  a gathered one and compare replies byte for byte, as CC-Q14 (a) did for
  the snapshot; a dropped-and-re-added column reads no old statistic
  (CY-R7).

### CY-S3 - column-group statistics (item 3)

- **A group is declared**: `GATHER STATISTICS ON t GROUP (a, b, ...)`
  creates the group object and gathers it; the group's key is the sorted
  column oids with their generations.
- **What it holds** `[design]`: the group's distinct-value count, a
  most-common-combinations list, and for each ordered pair inside it the
  degree of functional dependency (the fraction of rows whose `a` value
  determines its `b` value).
- **The estimator**: a conjunction of equalities that covers a group is
  priced from the group's list or its distinct count instead of the
  product of the columns' selectivities; a pair with dependency degree `d`
  is priced `sel(a) · (d + (1 − d) · sel(b))`; a multi-column join key that
  is a group uses the group's distinct count in the containment estimate.
  Conjuncts no group covers stay independent, as today.
- **Scope**: equalities only. A range over a correlated pair keeps the
  independence assumption, stated in `known-gaps.md` when S3 lands.
- **Cells**: a pair with `b = f(a)`; a pair that is independent (the group
  must not change the estimate beyond its band); a three-column group
  queried on two of its columns.

### CY-S4 - the management surface (item 5)

All admin-classed; a DBA and an agent use the same statements, and every
state is a `sys` view read by `SELECT` (BO-R3).

- **Lock**: `LOCK STATISTICS ON t` / `UNLOCK`. A locked object is replaced
  by no `GATHER`, no steward save and no `IMPORT` without `FORCE`.
- **Pending**: `GATHER ... PENDING` stores a new version unpublished;
  `PUBLISH STATISTICS ON t` makes it the planner's; `DISCARD PENDING`.
  `ADVISE PLAN ... USING PENDING` (CX-S7) shows which plans the pending
  model would choose, so the effect is seen before it is taken.
- **History and restore**: a replaced version is kept in
  `sys.statistics_history` (CY-Q12); `RESTORE STATISTICS ON t TO VERSION
  n` publishes it again as a new version.
- **Export and import**: `EXPORT STATISTICS ON t` returns one text
  document per object, naming relations and columns by name with their
  type descriptors (CY-Q9); `IMPORT STATISTICS ON t FROM '<document>'`
  resolves names to this instance's oids and refuses a column whose type
  descriptor differs. This is how a plan decision is reproduced on
  another instance or in a test.
- **Views**: `sys.statistics` (one row per object, its version, origin,
  state, age, sample), `sys.column_statistics` (the values, unpacked),
  `sys.statistics_history`.
- **`DROP TABLE`, `DROP COLUMN`** delete the objects that name them, in
  their own transaction; `DELETE STATISTICS ON t [(c, ...)]` is the
  explicit form.

### CY-S5 - the close

The measurement of §6, the owning specs (§7), CLAUDE.md's row.

## 4. Items

Every item is CLA's proposal and unmarked, except CY-Q0 and CY-Q1, marked
(a) by the operator (W8).

| item | question | options | CLA proposes |
|---|---|---|---|
| CY-Q0 | CY against CX-S0 | (a) CY-S1 is CX-S0's progress gate: `fixed` does not become the default until relation statistics survive a restart; (b) the two are independent | **(a), the operator's mark** (W8). (a): without it every restart fixes a fresh set of blind plans (CX-Q19) |
| CY-Q1 | the bar S0 must meet for S2 and S3 to be built | (a) at least one scenario statement whose order changes under true selectivities and runs 15 % or more faster at p50, CC's flip gate; (b) build regardless | **(a), the operator's mark** (W8). (a) for S2 and S3. S1 and S4's lock, export and import do not wait for it: persistence and reproducibility are W7's and CX's needs whatever S0 finds |
| CY-Q2 | who writes the persisted relation priors | (a) `GATHER`, and the steward when a prior's band moves, off the statement; (b) `GATHER` only | (a): under (b) an instance nobody has gathered on restarts blind for ever. A band moves rarely (PS5), so the write is rare; it is the one automatic write in CY, and it moves no captured plan |
| CY-Q3 | a live prior and a persisted object for one relation | (a) the newer one is published; a locked object always wins; (b) the gathered one always wins until re-gathered | (a): a gathered row count on a relation that has since doubled is the wrong one to keep, and a lock is how an operator says otherwise |
| CY-Q4 | sampling | (a) a whole walk by default, `SAMPLE n PERCENT` by leaf, stratified by the root's children as PS1 is; (b) sampled by default | (a): a whole walk is exact, yields (CY-R8), and is the operator's act to schedule; a sample's distinct count needs an estimator whose error S0 does not price |
| CY-Q5 | the histogram | (a) a most-common list of at most 100 values and an equi-depth histogram of at most 100 buckets, both sizes settable per `GATHER`; (b) a hybrid histogram that folds the two | (a): it is the simplest form that prices both a hot value and a range, and its two parts are inspectable separately |
| CY-Q6 | `char`/`varchar` columns | (a) NULL fraction, distinct count and most-common list only, the list holding at most the inline cell's bytes of each value; no histogram, a range keeps its constant; (b) a histogram over a fixed prefix | (a): a prefix histogram misprices every column whose values share a prefix, and the scenarios' string predicates are equalities |
| CY-Q7 | the walk's view | (a) a view per slice: no purge horizon is held for the length of the walk, and the counts are of a relation that moved under them; (b) one view for the whole walk, exact as of one instant, holding the instance's read horizon for its length | (a): a statistic is approximate by nature and changes no result (CY-R2), while (b) makes a gather on a large relation hold undo for minutes |
| CY-Q8 | does a `GATHER` publish at once | (a) yes, unless `PENDING` is written; (b) pending by default | (a): under `fixed` a publish moves no captured plan, and under `planned` the operator has chosen that plans follow statistics. `PENDING` is there for the one who wants to look first |
| CY-Q9 | the export document | (a) a line-oriented text form of this engine's own, versioned in its first line; (b) JSON | (a): the tree has no JSON reader or writer, and adding one for this is a dependency S4 does not need |
| CY-Q10 | persisting `card(T)` posteriors | (a) not in CY: they are keyed by statement and re-measured by the first complete run; (b) persisted with the priors | (a) until S0 shows a shape whose blind first order is wrong for lack of a posterior and not of a prior |
| CY-Q11 | the model's memory | (a) the published model is one immutable snapshot beside PlanStats', pinned by generation; a ceiling on its bytes, past which the least recently read column objects are not loaded and are priced by the constant, counted; (b) loaded on demand per relation | (a): one acquire load on a planned compile, nothing on a point read |
| CY-Q12 | history retention | (a) the last 8 versions per object; (b) by age | (a), provisional |
| CY-Q13 | expression and function conjuncts | (a) not in CY, the constant stays; (b) statistics on a Cabin's key function `F` | (a): (b) has a natural home (`cabin.md`'s `F`) and no measured consumer yet |

## 5. `[quiet-wrong]` risks

- **A statistic used as a fact about the data**: closed by CY-R1 and
  CY-R2. One reader, clamped at one row, no legality test.
- **A statistic inherited by another column**: closed by CY-R7.
- **An imported statistic bound to the wrong column**: closed by S4's
  name-and-type resolution at `IMPORT`; a mismatch refuses, never coerces.
- **A build chosen from an estimate too low**: not a wrong answer - the
  executor declines into the walk - but the costly direction; CY-R5 keeps
  CC's margin.
- **A gather's write racing a `DROP COLUMN`**: the result transaction
  re-reads the column's oid and generation under the relation's `IS`
  borrow and drops what no longer matches.

## 6. Measurement

AS-E epoch, `build-release`, results named by `git describe --tags`,
within the milestone's hour.

- **S0**: §3's premise file.
- **S1's gate**: the first planned statement after a restart runs the
  order the previous run ran, on scenarios 1-4.
- **The close**: the opening commit (A) against the close with an empty
  model (B) and with a gathered one (B'). Gates: B equals A within noise
  on every cell (CY-R4); a point read, a tuple key chain, a single-row
  `INSERT` and `UPDATE` do not move under B' (nothing on the write path,
  nothing before the tuple key chain's skip); the statements S0 named run
  their flipped order under B' with the gain S0 predicted. A `GATHER` of
  the largest scenario relation beside a point-read workload: the
  workload's p99 with and without it.

## 7. Contracts amended (when the stages land)

- `docs/spec/plan-stats.md` - §1's "memory-resident" narrowed to CC's
  three facts; the persisted model, its objects, bands and reader (S1-S3);
  PS9.
- `include/kds/exec/join_order.hpp` - the constants' comment: each is the
  price where no statistic is published for the conjunct.
- `docs/spec/catalog.md`, `docs/spec/sys-views.md` - `sys.statistics`,
  `sys.column_statistics`, `sys.statistics_history`; superblock 36.
- `docs/spec/backup.md` - the model is logged state and is restored.
- `docs/spec/alter.md`, `docs/spec/drop-table.md` - the objects a column
  or a relation takes with it.
- `docs/inflight/known-gaps.md`, "The join planner (CB)" - the entries on
  the blind prior and on unmeasured subsets, restated.
- `manual/` - `GATHER`, `LOCK`, `PUBLISH`, `EXPORT`, `IMPORT`, `RESTORE`
  and `DELETE STATISTICS`.

## 8. Stage log

Nothing has landed. CY is written and not opened. CY-Q0 and CY-Q1 marked
(a) on W8 (`raft-marks-2026-11-18.md` §6), documents only.
