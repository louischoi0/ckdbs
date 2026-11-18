# Work order CY - managed statistics: persisted, per column, per column group, and operated by a DBA or an agent

**Written 2026-11-18 and opened the same day on W9** (CY-S0 started), on worktree `cx-plan-change-option`
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
- **W9** (2026-11-18, `raft-marks-2026-11-18.md` §7): *"CY-Q2..Q13 $!a,
  proceed with CY-S0"* - every remaining item marked (a); CY opened,
  S0 started.
- **W11** (2026-11-18, `raft-marks-2026-11-18.md` §9): *"And applying
  statistics is lower priority: it must not affect the processing of
  OLTP business logic, or must keep that effect to a minimum."* -
  CY-R10.
- **W12** (2026-11-18, `raft-marks-2026-11-18.md` §10): *"CY-Q14..Q23,
  CX-Q28 $!a push no verify"* - every item the review and W11 raised
  marked (a).
- **W13** (2026-11-18, `raft-marks-2026-11-18.md` §15): *"For now, take
  this gate as not passed."* - CY-S2 and CY-S3 are not built.
- **W14** (2026-11-18, `raft-marks-2026-11-18.md` §16): *"For now, write
  every statistic as unlogged, and go on with: the statistics persisted
  to disk and the capture, the features around `CREATE PLAN`, the
  interface, and CX-S2 onward."* With W15, the operator's answers:
  unlogged statistics are kept across a clean shutdown only; plans,
  captured and administrator's, are logged. **CY-S1 opened.**

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
- `[source-read]` A column's oid does not name one column for ever: an
  `ADD COLUMN` that reuses a cleaned dropped slot takes that slot's oid
  and bumps `generation` (`docs/spec/alter.md` AL11).
- `[source-read]` A restore is mount recovery from the backup's start, so
  logged catalog state comes back (`docs/spec/backup.md`), and unlogged
  state is discarded at its mount (B6a).

## 2. Rules

- **CY-R1 - one reader** `[design]`. The persisted model is read by
  `exec::EstimateChain` and by `ANALYZE`'s estimates, as CC's facts are
  (`plan-stats.md` PS9), and so by every compile in which CX-R1 runs the
  search, and by `ADVISE PLAN`. No access-ladder rung, legality test or
  executor path reads it.
- **CY-R2 - a statistic never changes a result** `[quiet-wrong]`. No
  estimate declares a result empty, skips a step or shortens a walk. Every
  rows value is clamped at 1 (PS9), so a histogram bucket that holds no
  row prices one row and never none. A literal outside the histogram's
  whole range is CY-Q17.
- **CY-R3 - the persisted model changes only by an explicit act**
  `[design]`, from W7's deferral of item 4. `GATHER`, `IMPORT`, `RESTORE STATISTICS` and `PUBLISH`
  change it, and nothing else does, until item 4 is ruled on. The one
  exception CLA proposes is CY-Q2: the steward saving the relation priors
  it already computes, so that a restart does not start blind - beside
  a gathered or imported object, never in its place (CY-Q16 (a)).
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
  version that only grows and is logged with it. History can restore one
  (CY-S4), an export names what it exported, and a plan decision could
  name the versions it read if CX-Q25 is ruled that way.
- **CY-R7 - identity is by oid and generation, never by name**
  `[quiet-wrong]`. A column statistic is keyed by `(relation oid, column
  oid, column generation)`, so a column added into a reused slot (§1.4)
  misses the dropped column's statistic instead of inheriting it.
- **CY-R8 - gathering is a reader** `[design]`. `GATHER` walks as a read
  does - sliced, yielding, under the relation's `IS` borrow, at CY-R10's
  level - takes no `X`, blocks no writer of rows, and writes its result in
  one short transaction of its own at the end. **An `IS` held for a long
  walk makes every DDL that takes the relation's `X` wait and be refused
  `TxnConflict` at the 1 s fault net** (`txn.md` §5); how the borrow is
  held is CY-Q21. `GATHER` is admin-classed, autocommit only, refused
  inside `BEGIN` and on a `readonly` connection. It adds nothing to `INSERT`,
  `UPDATE` or `DELETE`.
- **CY-R9 - published values are banded** `[design]`. What the planner
  reads of a statistic is its half-octave band with CC's hysteresis
  (`plan-stats.md` PS5), so a re-gather that moves a value by a few
  percent publishes nothing and moves no plan. The stored value is what
  the gather computed; the band is the planner's view of it. What a band
  is for a list or a histogram, and what hysteresis does to an import, is
  CY-Q19.
- **CY-R10 - statistics work is behind OLTP** `[operator]` (W11). Every
  act that makes or applies a statistic - a `GATHER`'s walk and its
  result write, the steward's save (CY-Q2), the mount's seed, a publish,
  an `IMPORT` - is background work and gives way to a transaction's
  statements:
  - it runs at the `background` priority level whatever the session
    declared (`server/priority.hpp`; on the batch cores where there is
    more than one core), sliced and yielding as CY-R8 says;
  - it takes no lock a writer or a reader of the relation can wait on:
    the relation's `IS` borrow only, never `X`, never a row unit;
  - it adds nothing to `INSERT`, `UPDATE`, `DELETE` or a point read,
    and a publish costs a statement one acquire load, only on a
    planned compile (CY-Q11);
  - its page reads and its log writes are budgeted per step, so a
    `GATHER` on a cold relation does not turn the buffer pool over or
    queue behind a commit's sync;
  - when it cannot keep to that - the pool is full, the log is behind -
    it waits or stops with what it has; it is never the statement that
    is refused.
  The mount's seed reads `sys.statistics` rows and walks no relation,
  so it adds a catalog read to the mount and no more. **What "a
  minimum" is, as a number, is CY-Q14.**

## 3. Stages

A stage opens on the operator's word. S0 decides whether S2 and S3 are
built at all, each on its own bar (CY-Q15 (a)); S1 does not wait for it.
**S0 ran and the operator took the gate as not passed (W13, §8): S2 and
S3 are not built.** Their text below stands as the design a reopening
would start from.

### CY-S0 - the premise and the census (no behaviour change)

`build-release`, inside 20 minutes, results at
`bench/v3.4.12/results-cy-s0-premise-<describe>.md`.

- **How wrong the constants are**: for every planned statement of
  scenarios 1-4, each step's `est=` against `obs=` from `ANALYZE`
  (`plan-stats.md` §6), with the true selectivity of each literal conjunct
  computed by a `COUNT`.
- **Which plans it costs**: an oracle bound, because the cost model has
  no seam that takes a selectivity (`CostInputs`,
  `include/kds/exec/join_order.hpp:77-108`, carries rows and posteriors
  only). Every order of each statement is forced under `join_order =
  written`, CC-S0's method, and the fastest is set against the order the
  informed planner chooses - p50 and server CPU per statement.
- **Joint selectivity**: for every step with two or more literal conjuncts
  on one relation, the joint selectivity against the product of the
  single ones - S3's only premise.
- **What a blind first compile costs**: each shape's order from an empty
  snapshot against its order once priors are published - the population
  CX-Q19 is about.
- **The census**: every reader of the three constants, every place a
  column's type meets a comparison in the cost model, and what a gather
  walk can reuse of the scan kernel (BM) for decoding cells.

### CY-S1 - persistence (item 2)

- **Unlogged since W14, kept across a clean shutdown only (W15).**
  `sys.statistics` is written with no WAL record, under CR6 ("a catalog
  relation whose content is only a statistic is written unlogged",
  `docs/spec/crosscore.md` CC13), as `sys.access_stats` is. Every
  unlogged structure the engine has is discarded at every mount
  (`DiscardAdvisoryState`, `src/server/expeditor.cpp:1043`), because
  after recovery an unlogged page may name a page the repaired free map
  gave back. So statistics that outlive a restart are a new case, and
  its rule is the narrow one: **a shutdown that completes writes the
  statistics pages and marks them whole; a mount that finds the mark
  keeps them and clears it; a mount without it - after a crash, a
  restore, or a failed shutdown - discards them as it discards every
  other unlogged structure.** No recovery path reads a statistics
  page. What this gives up: after a crash or a restore the planner is
  blind until it observes again, which CX-Q19 (b)'s deferral covers;
  a lock, a pending version and the history are lost with the
  statistics, and `EXPORT`/`IMPORT` is how a model is carried across
  either. What it buys: no statistics write touches the log (CY-R10).
- **`sys.statistics`** `[design]`: an unlogged catalog relation (the next
  superblock version and the next free fixed root, coordinated with CX-S1's
  `sys.plans`), one row per statistics object, holding its key (CY-R7), kind
  (relation, column, column group), origin (`gathered`, `observed`,
  `imported`), version (CY-R6), state (`published`, `pending`, `locked`
  as a flag), the time it was made, the rows sampled, and its values as
  one spilled cell.
- **The relation object**: rows and leaves. `GATHER STATISTICS ON t`
  with no column list writes it from a whole walk - a count of a relation
  that moves under the walk (CY-Q7 (a)), not of one instant.
- **The mount seeds the snapshot before the listener opens** from the
  published relation objects, so the first compile after a restart or a
  restore prices from them, not from `kAssumedRelationRows`. The steward
  then runs as built; where a `gathered` or `imported` relation object
  exists the planner reads it, and the steward's live prior and its
  `observed` save serve a relation nobody gathered (CY-Q16 (a)).
- **The steward's own priors saved** (CY-Q2): when a relation's prior band
  moves, the steward stages an `observed` relation object, written off the
  statement by a `system` task. It replaces only an earlier `observed`
  object, never a `gathered`, `imported` or locked one (CY-Q16 (a)).
- **A restore** brings back no statistic (B6a discards unlogged state);
  the restored instance observes again, or takes an `IMPORT`.
- **Cells**: a clean restart plans a scenario join at the first
  statement as the previous run did; a crash and a restore each mount
  with no statistic and no error, the pages of the discarded relation
  given back; a `READ ONLY` session's planned
  statement appends no record; a volume at the previous superblock is
  refused.

### CY-S2 - column statistics (item 1)

- **`GATHER STATISTICS ON t (c, ...)`**, or `ALL COLUMNS` (the declared
  columns; a heap's hidden tuple key is never gathered or exported),
  `[SAMPLE n PERCENT]` (CY-Q4). Per column: the NULL fraction, the number of distinct
  values, a most-common-values list with frequencies, and an equi-depth
  histogram over the values the list does not hold (CY-Q5). A
  `char`/`varchar` column is CY-Q6.
- **The estimator** `[design]`, replacing a constant only where a column
  object is published (CY-R4):
  - `c = v`: `v`'s frequency if the list holds it; otherwise the non-NULL,
    non-listed fraction over the remaining distinct values;
  - a range end or `BETWEEN`: the histogram's fraction, interpolated
    inside a bucket for numeric, date and timestamp types, plus the listed
    values inside the range. A bucket whose two bounds are equal is not
    interpolated (no 0/0), and a `decimal128` bound is compared exactly
    and interpolated in the nearest double. A range on the tuple key
    keeps its span pricing (`join_order.cpp:151-152`). A literal outside
    the histogram's range is CY-Q17;
  - `IS NULL` / `IS NOT NULL`: the NULL fraction and its complement;
  - `c <> v`, `IN (...)`, `NOT IN (...)`: from the same parts;
  - a join-key equality with no measured `card(T)`: the product of both
    sides' rows over the larger distinct count, less each side's NULLs -
    the standard containment estimate - in place of today's `1 /
    max(rows)`, or `1 / rows(parent)` on a key join
    (`join_order.cpp:139-143`).
  A measured `card(T)` still outranks every estimate (PS2).
- **A value arriving as `?`** is a literal by the compile when it is
  bound at `C_BIND` (`kwp_session.cpp:580-585`). A compile that still
  holds an unbound `?` (`literals_known` false,
  `command_dispatcher.cpp:8909`) prices that conjunct from the column's
  average, `1 / distinct`, not from a value.
- **`ANALYZE`** names, per step, the source of each estimate: `const`,
  `bound`, `stat` or `obs`.
- **Cells**: the differential matrix - for every storable type and each
  comparison, the estimate against a `COUNT` on uniform, skewed (Zipf) and
  heavily-NULL data, within a stated factor; the four contract suites
  (waystone, index, cabin, inner-build) run under an empty model and under
  a gathered one and compare replies as CC-Q14 (a) set for the snapshot -
  as multisets where a statement has no `ORDER BY`; a dropped-and-re-added column reads no old statistic
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
  queried on two of its columns, which the group does not cover: those
  two stay independent unless a pair's dependency degree prices them.

### CY-S4 - the management surface (item 5)

All admin-classed; a DBA and an agent use the same statements, and every
state is a `sys` view read by `SELECT` (BO-R3).

- **Lock**: `LOCK STATISTICS ON t` / `UNLOCK`. A locked object is replaced
  by no `GATHER`, no steward save and no `IMPORT` without `FORCE`.
- **Pending**: `GATHER ... PENDING` stores a new version unpublished;
  `PUBLISH STATISTICS ON t` makes it the planner's; `DISCARD PENDING`.
  `ADVISE PLAN ... USING PENDING` (CX-S7) shows which plans the pending
  model would choose, so the effect is seen before it is taken.
- **History and restore**: a replaced `gathered` or `imported` version
  is kept in `sys.statistics_history` (CY-Q12); the steward's `observed`
  saves do not enter it, so they cannot push an operator's versions out; `RESTORE STATISTICS ON t TO VERSION
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
- **`DROP TABLE`, `DROP COLUMN`** delete the objects that name them and
  their history, in the DDL's transaction; `DELETE STATISTICS ON t [(c, ...)]` is the
  explicit form.

### CY-S5 - the close

The measurement of §6, the owning specs (§7), CLAUDE.md's row.

## 4. Items

Every item is the operator's mark, (a), each CLA's proposal: CY-Q0 and
CY-Q1 on W8, CY-Q2..Q13 on W9, CY-Q14..Q23 on W12. **Where a later item
amends an earlier one the later stands**: CY-Q15 over CY-Q1 (the gate is
CC's, on server CPU, with a bar each for S2 and S3), CY-Q16 over CY-Q2
and CY-Q3 (the steward's `observed` object never replaces a gathered or
imported one), CY-Q22 over CY-Q11 (the ceiling is enforced at the write
and nothing published is unloaded).

| item | question | options | CLA proposes |
|---|---|---|---|
| CY-Q0 | CY against CX-S0 | (a) CY-S1 is CX-S0's progress gate: `fixed` does not become the default until relation statistics survive a restart; (b) the two are independent | **(a), the operator's mark** (W8). (a): without it every restart fixes a fresh set of blind plans (CX-Q19), until CX-S1 persists captures. Read as: CX-S0 may be built, and `fixed` does not become the default before CY-S1 lands. It is necessary and not sufficient - a fresh volume and a relation created after mount have no persisted prior - so CX-Q19 (b)'s deferral is still needed as the fallback (review M6) |
| CY-Q1 | the bar S0 must meet for S2 and S3 to be built | (a) at least one scenario statement whose order changes under true selectivities and runs 15 % or more faster at p50, CC's flip gate; (b) build regardless | **(a), the operator's mark** (W8). (a) for S2 and S3. S1 and S4's lock, export and import do not wait for it: persistence and reproducibility are W7's and CX's needs whatever S0 finds **Amended by CY-Q15 (a) on W12.** |
| CY-Q2 | who writes the persisted relation priors | (a) `GATHER`, and the steward when a prior's band moves, off the statement; (b) `GATHER` only | **(a), the operator's mark** (W9). (a): under (b) an instance nobody has gathered on restarts blind for ever. A band moves rarely (PS5), so the write is rare; it is the one automatic write in CY, and it moves no captured plan **Amended by CY-Q16 (a) on W12.** |
| CY-Q3 | a live prior and a persisted object for one relation | (a) the newer one is published; a locked object always wins; (b) the gathered one always wins until re-gathered | **(a), the operator's mark** (W9). (a): a gathered row count on a relation that has since doubled is the wrong one to keep, and a lock is how an operator says otherwise **Amended by CY-Q16 (a) on W12.** |
| CY-Q4 | sampling | (a) a whole walk by default, `SAMPLE n PERCENT` by leaf, stratified by the root's children as PS1 is; (b) sampled by default | **(a), the operator's mark** (W9). (a): a whole walk is exact, yields (CY-R8), and is the operator's act to schedule; a sample's distinct count needs an estimator whose error S0 does not price |
| CY-Q5 | the histogram | (a) a most-common list of at most 100 values and an equi-depth histogram of at most 100 buckets, both sizes settable per `GATHER`; (b) a hybrid histogram that folds the two | **(a), the operator's mark** (W9). (a): it is the simplest form that prices both a hot value and a range, and its two parts are inspectable separately |
| CY-Q6 | `char`/`varchar` columns | (a) NULL fraction, distinct count and most-common list only, the list holding at most the inline cell's bytes of each value; no histogram, a range keeps its constant; (b) a histogram over a fixed prefix | **(a), the operator's mark** (W9). (a): a prefix histogram misprices every column whose values share a prefix, and the scenarios' string predicates are equalities |
| CY-Q7 | the walk's view | (a) a view per slice: no purge horizon is held for the length of the walk, and the counts are of a relation that moved under them; (b) one view for the whole walk, exact as of one instant, holding the instance's read horizon for its length | **(a), the operator's mark** (W9). (a): a statistic is approximate by nature and changes no result (CY-R2), while (b) makes a gather on a large relation hold undo for minutes |
| CY-Q8 | does a `GATHER` publish at once | (a) yes, unless `PENDING` is written; (b) pending by default | **(a), the operator's mark** (W9). (a): under `fixed` a publish moves no captured plan, and under `planned` the operator has chosen that plans follow statistics. `PENDING` is there for the one who wants to look first |
| CY-Q9 | the export document | (a) a line-oriented text form of this engine's own, versioned in its first line; (b) JSON | **(a), the operator's mark** (W9). (a): the tree has no JSON reader or writer, and adding one for this is a dependency S4 does not need |
| CY-Q10 | persisting `card(T)` posteriors | (a) not in CY: they are keyed by statement and re-measured by the first complete run; (b) persisted with the priors | **(a), the operator's mark** (W9). (a) until S0 shows a shape whose blind first order is wrong for lack of a posterior and not of a prior |
| CY-Q11 | the model's memory | (a) the published model is one immutable snapshot beside PlanStats', pinned by generation; a ceiling on its bytes, past which the least recently read column objects are not loaded and are priced by the constant, counted; (b) loaded on demand per relation | **(a), the operator's mark** (W9). (a): one acquire load on a planned compile, nothing on a point read **Amended by CY-Q22 (a) on W12.** |
| CY-Q12 | history retention | (a) the last 8 versions per object; (b) by age | **(a), the operator's mark** (W9). (a), provisional |
| CY-Q13 | expression and function conjuncts | (a) not in CY, the constant stays; (b) statistics on a Cabin's key function `F` | **(a), the operator's mark** (W9). (a): (b) has a natural home (`cabin.md`'s `F`) and no measured consumer yet |
| CY-Q14 | the bar for CY-R10 (W11): how much a running `GATHER` may move an OLTP workload | (a) the workload's p50 and p99 (point read, single-row `INSERT` and `UPDATE`, a three-relation tuple key join) with a `GATHER` of the largest scenario relation running stay inside the run's noise floor where the instance has a batch core, and within 5 % at `cores = 1`, `[provisional]` until S2's first cell measures it; a stage that misses it does not close; (b) no numeric bar, the mechanisms of CY-R10 only | **(a), the operator's mark** (W12). (a): W11 is a requirement on an effect, and an effect is met by a measurement, not by a list of mechanisms. |
| CY-Q15 | **CY-Q1's gate, put back** (review H2). CY-Q1 (a) calls "15 % or more faster at p50" CC's flip gate; CC's gate is 15 % of **server CPU per statement in each of 3 interleaved pairs** (`workorder-cc-plan-statistics.md:360`), which is also CLAUDE.md's metric for a per-statement cost. And S0's single-conjunct selectivities are evidence for S2 only | (a) the gate is CC's, on server CPU; S2 needs one scenario statement that meets it for want of a single column's selectivity, S3 one that meets it for want of a joint one (S0's joint-selectivity table); (b) p50, as CY-Q1 (a) was worded, one bar for both | **(a), the operator's mark** (W12). (a). S0 reports both metrics, so the answer costs no re-run. |
| CY-Q16 | **CY-Q2 and CY-Q3, put back** (review H1). As marked, the steward's sampled prior - which PS1's own text says read a height-3 tree at 0.38x before stratifying - replaces a just-gathered row count whenever it is newer and in another band, and an imported model is overwritten by the target's live prior. That is an automatic re-gather of the relation object, the half of item 4 the operator deferred | (a) the steward's `observed` object is saved beside a `gathered` or `imported` one and never replaces it; the planner reads the gathered one while it exists; whether `observed` may ever supersede it joins item 4; (b) as marked, the newer wins | **(a), the operator's mark** (W12). (a). It keeps CY-Q2's purpose - a restart does not start blind where nobody gathered - and keeps CY-R3 true. |
| CY-Q17 | a literal outside a histogram's range (review H4). With no re-gather, an append-mostly column - a timestamp, an issued key - leaves its histogram's range the day it is gathered, and CY-R2 as first written priced "recent rows" at one row: the costly direction, by more than CY-R5's margin covers | (a) out of range is priced by the constant, as if no statistic existed (CY-R4); (b) extrapolated from the live row prior's growth since the gather; (c) one row | **(a), the operator's mark** (W12). (a): it is never worse than today. (b) is the better estimate and needs item 4's notion of growth since a gather |
| CY-Q18 | value-dependent pricing under `fixed` (review H3). Today an equality costs 0.01 whatever the literal, so a captured order barely depends on the first binding. With a most-common list the first binding's value - hot or rare - decides the order every binding runs (CX-Q13 (a)) | (a) a compile that will be **captured** prices an equality from the column's average, `1 / distinct`, and a range from its span band, never from the literal's own frequency; `planned` and `ADVISE PLAN` price the literal; (b) always price the literal | **(a), the operator's mark** (W12). (a): the capture is one plan for every binding, so it is priced for the average binding. A shape whose hot and rare values need different orders is what an administrator's `VARIANT ... WHEN ARG(k)` is for, and `ADVISE PLAN` shows it |
| CY-Q19 | what a band and a publish are (review M1, M2, M3) | (a) the stored object is unbanded; the band with hysteresis applies to each **derived estimate** the planner reads (a selectivity, a distinct count), computed at publish; an `IMPORT`, a `RESTORE` and a mount publish fresh, with no hysteresis carried, so equal stored values give equal bands on every instance; only a committed gather or import is published; the steward's step rebuilds the model snapshot and bumps one model generation, which `PlanMemo`'s key gains; a `sys.statistics` write does not move the schema version word (CX-Q23's answer, for the same reason); a block is `informed` when a prior, a posterior **or a column statistic** priced it; (b) band the stored values | **(a), the operator's mark** (W12). (a): under (b) two instances holding one export publish different models, which defeats S4's reason for export. "Publish" then has one meaning: the object state `published` is what the next snapshot rebuild reads |
| CY-Q20 | a gather's memory (review H5). An exact distinct count, common-value list and equi-depth histogram over a whole walk need memory in proportion to the distinct values, where every other engine structure is capped (`sort_max_rows`, `join_build_max_rows`) | (a) bounded sketches in one pass - a distinct-count sketch, a top-k counter, a quantile sketch - under a per-gather byte cap, the stored object carrying each one's error bound; (b) exact, refusing a column past a cap; (c) two passes | **(a), the operator's mark** (W12). (a): it fits CY-R10, and a statistic was never exact (CY-Q7 (a)). CY-Q4 (a)'s reason "a whole walk is exact" falls with it; its choice - a whole walk by default - stands on the other reason, that a sample's distinct count is the least reliable estimate |
| CY-Q21 | how a gather holds the relation (review M4, W11) | (a) the `IS` borrow per slice: released at each yield, so a DDL waiting for `X` passes; the gather re-binds and stops with nothing published if the relation, a gathered column or its generation changed; (b) one `IS` for the whole walk, refusing every `X` DDL on the relation for its length | **(a), the operator's mark** (W12). (a): under (b) a gather on a large relation makes `DROP TABLE`, `ADD`/`DROP COLUMN`, `CREATE ASSERTION`, a unique index build and `REINDEX` fail at the 1 s fault net for minutes, which is statistics work in front of the operator's own DDL |
| CY-Q22 | the model's ceiling, put back (review M5). CY-Q11 (a) as marked unloads the least recently read column objects past the ceiling and prices them by the constant, so a plan depends on what was read lately and a `fixed` capture can be made blind because an object was evicted | (a) the ceiling is enforced at the write: a `GATHER` or `IMPORT` that would pass it is refused naming the setting, nothing published is ever unloaded; (b) as marked, unload | **(a), the operator's mark** (W12). (a): one catalog and one model generation then give one plan. |
| CY-Q23 | the mount's seed (review L6, W11) | (a) the seed reads relation objects only - one row each, no spilled cell - and column objects load after the listener opens, a compile before then pricing by the constant and not being captured (CX-Q19 (b)); (b) everything before the listener | **(a), the operator's mark** (W12). (a): the mount's added time is then one catalog chain's read, bounded by the relation count, not by the model's size |

**CY-Q24..Q30** were raised by CY-S1b and CY-S4's build and are **adopted on
the operator's standing go-ahead of 2026-11-18 (go-ahead-achieving-milestone)**,
each as CLA proposed (`raft-marks-2026-11-18.md` §48) - not the operator's
per-item marks. None is `[quiet-wrong]`: a relation object prices a join
order and nothing else (CY-R1, CY-R2).

| item | question | options | CLA proposes |
|---|---|---|---|
| CY-Q24 | what S1b and S4 build once W13 took S2 and S3 out | (a) the relation object alone - rows and leaves - with the whole of S4's surface over it; a column list, `ALL COLUMNS`, `GROUP` and `SAMPLE` refused `NotImplemented` naming the ruling; no `sys.column_statistics`; (b) the column surface built empty | **(a), adopted on the standing go-ahead.** (a): W13's "not built" is the operator's; an empty surface is a spelling accepted and nothing enforced |
| CY-Q25 | where an operator's object lives during a run, under W14 and W15 | (a) in memory, one store for the instance beside the steward's facts (`stats::ManagedStatistics`, latched), written into `sys.statistics` by the shutdown tail alone (PS15) and read back by the next mount (PS16), so a crash or a restore loses it; (b) a page write at each act | **(a), adopted on the standing go-ahead.** (a): any mount after a crash discards the chain whatever a run wrote to it (PS16), so a write during the run buys nothing and costs a page the workload pays (CY-R10). §3's "one short transaction of its own at the end" (CY-R8) has nothing to transact: no act here is logged or undone, which is why every one is refused inside `BEGIN` |
| CY-Q26 | what a relation object counts | (a) every leaf's live slots - read, not delete-marked - and the leaves, through the sibling chain one page at a time, each checked to be a leaf the relation owns: PS1's quantity, with no view and no purge horizon; (b) the rows visible to a per-slice view | **(a), adopted on the standing go-ahead.** (a): the gathered count and the steward's observed one are then one quantity, so one replacing the other moves no plan by a change of definition; an undecided insert is counted and an undecided delete is not subtracted, inside CY-Q7 (a)'s "a relation that moved under the walk" |
| CY-Q27 | what CY-R10's level means for a `GATHER` | (a) its walk slices at the `analytic` slice (16 leaves, `PagesPerSlice`) whatever level the session declared, and stays on the session's core: a session moves between cores only at a transaction head (BL-S3), and a statement cannot move mid-walk; its page reads come through the pool; both stated in `known-gaps.md`; (b) move the session to a batch core mid-statement | **(a), adopted on the standing go-ahead.** (a): (b) needs a session to cross cores inside a statement, which no statement does. A DBA who wants the batch cores declares `SET PRIORITY background` first, as for any statement. CY-Q14's measurement prices what is left |
| CY-Q28 | what a lock stops, and the refusals' codes | (a) a lock is on the relation's slot, object or no; every act that would change the published object - `GATHER` and `IMPORT` without `PENDING`, `PUBLISH`, `RESTORE`, `DELETE STATISTICS` - is refused `InvalidArgument` on a locked slot unless `FORCE` is written; a pending `GATHER` or `IMPORT` and `DISCARD PENDING` are admitted; `DELETE` keeps the lock and the version counter, so a later version is still newer (CY-R6); inside `BEGIN` every statement is refused `NotImplemented` as `DROP PLAN` is; a catalog relation is `Unsupported`; (b) a lock stops `GATHER` and `IMPORT` only | **(a), adopted on the standing go-ahead.** (a): §3's "replaced by no `GATHER`, no steward save and no `IMPORT` without `FORCE`" names the replacing acts S4 had then; `PUBLISH`, `RESTORE` and `DELETE` replace the published object as surely |
| CY-Q29 | the export's surface and document (CY-Q9 (a)'s form) | (a) `EXPORT STATISTICS ON t` is one `SELECT` over `sys.statistics`' `document` column, the row the planner reads (`used = 1`) - the gathered or imported version, or the steward's prior as `origin=observed` where none is - so it answers as every report does (SV1); the document is one line, `KDS-STATISTICS 1 RELATION rows=<n> leaves=<n> origin=<o> version=<n> made_at=<us> relation=<ns.t>`, and `IMPORT` takes `rows` and `leaves`, reads the rest as provenance and refuses a field it does not know; (b) a reply line carrying the document | **(a), adopted on the standing go-ahead.** (a): a KWP client reads rows as it reads every view; a column object, if S2 is ever built, is one more kind on the same first line |
| CY-Q30 | `ADVISE PLAN ... USING PENDING` (§3's S4) | (a) not built in CY: `ADVISE PLAN` (CX-S7) is not on `main` at CY-S1b's opening commit `9712f5f0`, and building it here crosses into CX; stated in `known-gaps.md`; (b) build it here | **(a), adopted on the standing go-ahead.** (a): the skill this session runs does not cross into a second milestone; CX's close is where the clause belongs |

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
  the largest scenario relation beside the OLTP cells of CY-Q14: each
  cell's p50 and p99 with and without it, against CY-Q14's bar (CY-R10).

## 7. Contracts amended (when the stages land)

- `docs/spec/plan-stats.md` - §1's "memory-resident" narrowed to CC's
  three facts; the persisted model, its objects, bands and reader (S1-S3);
  PS9.
- `include/kds/exec/join_order.hpp` - the constants' comment: each is the
  price where no statistic is published for the conjunct.
- `docs/spec/catalog.md`, `docs/spec/sys-views.md` - `sys.statistics`,
  `sys.column_statistics`, `sys.statistics_history`; the superblock
  version.
- `docs/spec/backup.md` B6a, `docs/spec/crosscore.md` CC13 - the model is
  unlogged state: kept across a clean shutdown, discarded at every other
  mount, never restored.
- `docs/spec/alter.md`, `docs/spec/drop-table.md` - the objects a column
  or a relation takes with it.
- `docs/inflight/known-gaps.md`, "The join planner (CB)" - the entries on
  the blind prior and on unmeasured subsets, restated.
- `manual/` - `GATHER`, `LOCK`, `PUBLISH`, `EXPORT`, `IMPORT`, `RESTORE`
  and `DELETE STATISTICS`.

## 8. Stage log

CY-Q0 and CY-Q1 marked (a) on W8 (`raft-marks-2026-11-18.md` §6);
CY-Q2..Q13 marked (a) and **CY opened on W9** (§7). **CY-S1 opened on W14**
(§16), with the statistics unlogged.

- **CY-S0** started 2026-11-18 on worktree `cx-plan-change-option`. No
  engine code.

### CY-S0, 2026-11-18: the gate is not met on the scenarios; what a blind compile costs is measured; two cases are owed

`[measured]` on `v3.4.12-18-ge0cd5a93`, `build-release`,
`bench/v3.4.12/results-cy-s0-premise-v3.4.12-18-ge0cd5a93.md`, 01:34 to
01:52 UTC, inside the 20 minutes. An oracle bound: every order forced under
`join_order = written` against the order the informed planner chose.

- **The gate (CY-Q15 (a)) is not met, for S2 or for S3.** Over 15 cells -
  five scenario join statements at 200, 1,000 and 10,000 rows - the
  informed planner's order is the fastest forced order in 14. **Every
  forced order that differs from the planner's**, not only the fastest,
  has 15 % less server CPU in 0 of 4 rounds and a 15 % lower p50 in 0 of
  4 rounds, in every cell (the review's recomputation from the raw file).
  The one cell whose pooled p50 shows 20 % (`cross-join-200`) is not a
  pass by either metric: per round the two orders' p50s differ by at most
  3 µs (129 / 126 / 81 / 80 against 129 / 129 / 80 / 81), and the pooled
  gap is a shift every arm took between rounds 2 and 3.
- **The constants the model applies are wrong by 0.08x to 12.5x**
  (constant over true selectivity: `model_id = k` 0.08 at every size,
  `session_no = k` 12.5 at 10,000 rows). A range on the tuple key is not
  priced by a constant at all - the span's band prices it
  (`src/exec/join_order.cpp:152-153`) - so `u.id <= 16` is not an error of
  the model.
- **After a complete run, the executed order's prefixes are priced from
  what was observed** - `est=` is `obs=` rounded to its half-octave band,
  up to 19 % off (11,585 against 10,000) - **and every other order's
  prefix keeps the constants at every compile** (`known-gaps.md`, "The
  join planner (CB)"). So the constants do go on pricing the orders that
  were not run; on this data that did not hide a faster order.
- **S3 has no premise from the scenarios.** No scenario statement carries
  two literal conjuncts on different columns of one relation; the one
  pair is a tuple key `BETWEEN`, which the model prices as one span.
- **A blind first compile costs** (CX-Q19, CY-S1). A fresh server, every
  relation at 100,000 rows and no posterior, searched and kept the written
  order for four of the five statements (`switch=1`: the cheapest under
  those inputs). Two of those are the slow order: `customer-statement`
  (`freights` first) runs 1.75x, 2.9x and 2.2x the informed order's p50
  (131 / 500 / 4,186 µs against 75 / 171 / 1,919), `compare-all` 1.22x,
  1.17x, 1.05x. **The run does not say which missing fact causes it**: the
  blind arm lacked the priors and the posteriors and applied the 0.01
  constant.
- **Not measured, stated as such**:
  - **the compile `fixed` will capture once CY-S1 is built** - relation
    priors published, no posterior yet for the shape. It decides two
    things S0 leaves open: whether persisted priors alone repair the blind
    order, and whether a constant then picks a wrong one - the one compile
    in which a column statistic could act under `fixed`;
  - **a skewed column**: the scenario columns are uniform (one value in
    k), and the optional synthetic Zipf cell was cut for the budget;
  - the scenario drivers' own data and sizes (the data is synthetic to
    the schemas' shapes, as CC-S0's was) and a second literal per cell.
- **Limits of the run's validity**, from the review:
  - a two-state shift on the host, about +55 µs of latency and +20-25 µs
    of server CPU per statement at 200 rows, hit every server and
    sometimes flipped inside a block. The three servers shared one CPU
    and the client was not pinned; the cause is not verified. The verdict
    survives it because every losing order loses in every round;
  - the planner arm is slower than its own forced order beyond the noise
    floor in six cells (its planning, memo and steward CPU), which biases
    toward finding a flip, so the "no" is conservative;
  - blocks were about 0.3 s, four rounds;
  - **the worktree's HEAD moved during the stage**: this session committed
    documents and, to land them on W12, checked `origin/main` out, merged
    and checked the branch back out at 01:39:46-01:40:06, so the measured
    run started with HEAD at `1ba204ab`. `git diff --stat e0cd5a93
    1ba204ab` touches `docs/` and `instructions/` only: the engine and the
    tools are the measured commit's. A measurement and a landing do not
    share a worktree again.
- **The census** `[source-read]` at `e0cd5a93`:
  - the three selectivity constants have one reader, the conjunct loop of
    `src/exec/join_order.cpp:109-185` (equality `:160`, a range end
    `:162`, other `:124`, `:149`, `:164`, `:171`, a key set `:182`);
    `kAssumedRelationRows` is read at `src/exec/step_compiler.cpp:2431`
    and as a fallback at `join_order.cpp:58` and `:63`;
    `tests/join_planner_test.cpp` names them four times;
  - **the loop never reads a literal's value**: it prices by the
    operator and the column position, and the only values it reads are a
    tuple key range's span (`step.range`) and a key set's size. An
    estimator that looks a value up in a list or a histogram is the first
    reader of a literal in the cost model, and needs the value in the
    column's type - what `CompareValues` (`include/kds/exec/row_codec.hpp:398`)
    already compares;
  - a gather walk decodes cells through `storage::DecodeCell`
    (`include/kds/storage/tagged_cell.hpp:183`) and can slice as a build's
    walk does (64 pages, `docs/spec/sched.md` §3).

**The review of S0's results** (`critics-developer`, at `fc705f97`)
recomputed the latency table, the noise floors, the round counts and the
blind ratios from the raw file and found them to match; it found the
verdict to hold and five statements around it stronger than the data, each
corrected above and in the results file: that `est=` equals `obs=`, that a
constant prices only a first compile, the 187x error (a constant the model
never applies), the blind cost attributed to the relation prior alone, and
a clock-skew note that was this session's own checkout.

**What follows** `[design]`, for the operator: by the gate as marked, S2
and S3 are not built on this evidence, and S1 and S4's lock, export and
import go on, as CY-Q1 (a) and CY-Q15 (a) say. The verdict is about the
scenarios as much as about the engine - they hold no skewed column and no
correlated pair - and it leaves the priors-only compile unmeasured.

**The operator's verdict** (W13, `raft-marks-2026-11-18.md` §15): the
gate is taken as not passed. **CY-S2 (column statistics) and CY-S3
(column groups) are not built.** What stands of CY is S1 (persistence)
and the part of S4 that does not need a column object - lock, pending
and publish, history and restore, export and import of relation
objects. CY-Q4..Q6, CY-Q13, CY-Q17, CY-Q18 and CY-Q20, and the column
half of CY-Q19 and CY-Q22, have nothing to apply to while S2 is not
built; they stay as marked for an order that reopens it. The two cases
S0 did not measure - a skewed column, and the priors-only compile - are
what a reopening would rest on. The priors-only compile is also S1's
own premise and is measured as S1's first step.

### CY-S1a, 2026-11-18: relation priors across a clean shutdown

Built on worktree `cx-plan-change-option` from `7bdb03b4`. **S1 is split**,
adopted on the standing go-ahead: S1a is the storage, the shutdown's write
and the mount's read; S1b is `GATHER STATISTICS ON t` for a relation and
the `sys.statistics` view. S1a alone is what CY-Q0 gates CX-S0 on.

- **The premise, before the build** (CLAUDE.md, "re-measure a premise"):
  `StatisticsPlannerTest.RelationPriorsAloneFixTheFilteredJoinsBlindOrder`.
  With the two relations' priors and no posterior, scenario 2's
  customer-statement puts the filtered `cargos` first at each of CY-S0's
  three sizes, where blind it keeps `freights` first. `compare-all` had the
  same cell since CC-S3. So a prior is enough and CY-Q10 (a) stands.
- **What was built**: `sys.statistics` (oid 160, root page 17, superblock
  36), unlogged under CR6; `Catalog::TakeStatisticsAtMount` and
  `WriteStatisticsAtShutdown`, with `DiscardAccessStats`' walk factored into
  `DiscardUnloggedChain` for both relations; `PlanStats::SeedPriors` and
  `ExportPriors`; the mount's take beside the advisory discard and the
  shutdown tail's write before its final sync (`docs/spec/plan-stats.md`
  §8, PS13-PS17).
- **Narrower than §3's S1 text, and why**: the relation is written **at
  shutdown only**, not when a band moves (CY-Q2 (a)'s steward save). W15
  keeps a statistic across a clean shutdown only, so a write during the run
  would be discarded by the only mount that could follow it without one;
  and with no write during the run, statistics cost a running workload no
  page and no log record (CY-R10). The row holds the prior's band, so a
  prior comes back at the band it left at. It carries nothing else: an
  object's value, version and lock (CY-R6, S4) get fields when a stage
  reads them, which costs no migration since a mount empties the chain.
- **The rule the storage class rests on** (PS16): a mount believes the
  chain only on a closing mark and its count, and empties it whatever it
  found, inside the discard window, so the emptying is on the device
  before the listener binds.
- **Cells**: 16 new in `tests/statistics_persistence_test.cpp` and one in
  `tests/join_planner_test.cpp`, among them a clean stop kept for
  exactly one mount, a crash after the mount that took the priors, a crash
  with no stop, rows with no mark, a mark that counts more rows than the
  chain holds, a link to a page that is not there. The golden log is
  re-pinned (bootstrap's two rows for the new relation join the stream).
- **Not built in S1a, stated**: no SQL statement reads or gathers the
  relation; the sim's shutdown does not call the write, so the simulation
  corpus does not exercise it; `IMPORT`/`EXPORT` do not exist, so after a
  crash or a restore the planner is blind until it observes.
- **Overhead not measured; measured at the milestone's close.**
- **The review** (`critics-developer`, at `abe870fc`) attacked PS16 and
  found the mark-and-count rule sound and the mount's emptying on the
  device before any statement. It found one defect that would have
  shipped, **confirmed by a probe**: the mount's delete-mark sweep walks
  every catalog root, unguarded, before the discard window, so a shutdown
  killed between page 17's write and the map's left a link that refused
  that mount and every mount after it. Fixed: the sweep passes an unlogged
  relation by (`catalog::IsUnloggedCatalogRelation`) - which closes the
  same hole `sys.access_stats` already had, and makes PS15's "no tick
  touches the pages" true. Also taken: an unreadable head is made again
  empty where it was a refused mount (for `sys.statistics`;
  `sys.access_stats` keeps its stated refusal); a carried prior is due a
  seed by a flag, not by a zero timestamp that a clock under ten seconds
  would not pass; the take and the discard are one walk; the row lost
  four fields nothing read (`rows`, `made_at`, `version`, `origin`) and
  the expeditor a member and a clock read; `ExportPriors` is `Publish`'s
  loop; four stale root-range mentions. Five mutants, each killed: the
  sweep's skip removed, the head not healed, the count ignored, the mark
  ignored, a cut chain believed.
  Not taken: nothing was rejected. **Left open, stated** (`known-gaps.md`,
  "Plan statistics (CY)"): no cell drives `RunUntilStopped`'s own call of
  the write; no cell crashes between the shutdown's page writes and its
  map write on a device (the in-memory cells stand for it); the sim does
  not exercise the path.

**The review of this order** (`critics-developer`, read at `fb65d7bd`,
reported after W8's push and W9's marks). It verified the citations of
§1 and found five wrong, each corrected: CC-Q14 (a) is a multiset
comparison, not byte for byte; CC's flip gate is server CPU per statement,
not p50 (CY-Q15); B6a is the discard of unlogged state, not the replay;
an unbound `?` reaches a compile, so the estimator does not always see a
value; and the selectivity seam S0 named does not exist, so S0 measures
an oracle bound. Taken as items: H1 (CY-Q16), H2 (CY-Q15, and S0's joint
selectivity), H3 (CY-Q18), H4 (CY-Q17), H5 (CY-Q20), M1-M3 (CY-Q19), M4
(CY-R8, CY-Q21), M5 (CY-Q22), L6 (CY-Q23). Taken as text: M6 on CY-Q0's
row, M8 in CY-R6, L1-L5 and L7-L9, CY-R3 retagged `[design]`, CY-R1's
list cut. **Three findings change an item the operator had marked
(CY-Q1, CY-Q2 with CY-Q3, CY-Q11); the marks are left as marked and the
change is put back as CY-Q15, CY-Q16 and CY-Q22.** The operator marked
all three (a) on W12, with CY-Q14 and CY-Q17..Q23. Not taken: the cuts it
proposed to §5's bullets and to the repeated statement that item 4 is
out - §5 is the order's list of `[quiet-wrong]` risks and is read alone;
and CY-Q5's per-gather sizes stay, being the operator's mark.

### CY-S1b and CY-S4's relation-object surface, 2026-11-18: built

Built on worktree `cy-managed-statistics` from `origin/main` at `9712f5f0`
(`v3.4.12-*`), on the operator's standing go-ahead (`raft-marks-2026-11-18.md`
§48); CX's worktree is another live session's, so CY took one of its own.
CY-Q24..Q30 adopted as CLA proposed (§4).

- **What was built**: `stats::ManagedStatistics` (PS18) - per relation a
  published, a pending and the last 8 replaced versions, a lock and a
  counter; `PlanStatsSnapshot`'s managed overlay, read before the steward's
  prior (PS19), and `PlanStats::RepublishManaged`; `stats::RelationGather`,
  the leaf-chain count (PS20), driven by the dispatcher's `GatherRun` - a
  `BuildRun` with no transaction, a read borrow per slice, 16 leaves a
  slice; `GATHER`, `LOCK`, `UNLOCK`, `PUBLISH`, `DISCARD PENDING`,
  `RESTORE`, `EXPORT`, `IMPORT`, `DELETE STATISTICS` (PS21-PS22), as
  dispatcher verbs that reserve no word and move no fingerprint;
  `sys.statistics` and `sys.statistics_history` (PS23); the widened
  `SysStatisticsRow` and the shutdown's and mount's round trip (PS24); a
  committed drop forgets the relation's objects.
- **Narrower than §3, and why**: no column objects, no `SAMPLE`, no
  `sys.column_statistics` (W13, CY-Q24); no steward save of an `observed`
  object during the run - the steward's priors were already written at
  shutdown by S1a, which is CY-Q2's purpose under W15; no result
  transaction (CY-Q25); no `ADVISE PLAN ... USING PENDING` (CY-Q30). §3's
  "keyed by `(relation oid, column oid, column generation)`" (CY-R7) has no
  column key to carry; a relation object's key is its oid, never reissued.
- **Cells**: `tests/managed_statistics_test.cpp`, 19: the store's versions,
  history, pending, lock, restore, delete, forget and bands; the snapshot's
  precedence; the rows' round trip, the history's order across an unordered
  chain; through a dispatcher - a gather's exact count and leaves, a sliced
  gather yielding once per 16 leaves through `DispatchAsync`, every
  statement's refusals at their bytes, a lock, export and import, restore
  and delete in the views, the role; a whole instance - a clean stop keeps
  every object and its counter, a crash keeps none, a committed drop
  forgets. `statistics_persistence_test.cpp`'s writes go through the new
  row.
- **The review** (`critics-developer`, on the uncommitted tree over
  `9712f5f0`) found the latch order, the walk across yields, the routing and
  the role sound, and two defects, both fixed by it: the replaced versions
  came back from a mount in version order, where the history is in
  replacement order (a `PUBLISH` of an older pending version breaks the
  equation) - a replaced row now carries its place in `count`; and a gather
  over an *imported* version kept the hysteresis PS19 gives a gather over a
  gather only. It also found the hysteresis cell testing nothing (1000 and
  1150 share a band either way; now 1260). Taken after it: a byte past an
  escaped `''` in a document drifted one per escape (now mapped); a field
  named twice is refused; the steward's and the managed installs share
  `InstallLocked`; a fresh band is `BandOf`; a drop moves the generation only
  where a published version went with it, so a drop of an unmeasured
  relation invalidates no core's memo; the lock's refusal has one source
  (`CheckPublishable`); the unreachable `kBtree` test is gone and the
  dropped-mid-walk branch's message says what refuses the result.
  **Rejected**, with reasons: merging the store's `Slot` into
  `StatisticsSlotView` - the view is the export boundary and the store keeps
  its own container; a helper for the export's `SelectStmt` - none exists
  and one caller does not earn it; a restored gathered version counting as
  "not a gather" for the next gather's hysteresis - a restore publishes
  fresh already, and the next gather over it is a gather over a gathered
  value, which PS19 names. **Left open, stated**: no cell cancels a gather
  or drops its relation between two slices.
- **Overhead not measured; measured at the milestone's close.**
