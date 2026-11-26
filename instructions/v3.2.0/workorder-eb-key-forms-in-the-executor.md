# Work order EB - the key forms in the executor: a key range that is a range, a join by a whole key, an order the rows already hold, an upsert by any key

Status: **WRITTEN 2026-11-26, not opened.** Written on worktree
`known-gaps-milestone-orders` (branch
`worktree-known-gaps-milestone-orders`) from `8274c951`
(`v3.4.17-148-g8274c951e`) on the operator's word of 2026-11-26
(`raft-marks-2026-11-26.md` §3). No stage is started; EB-S0 waits for the
operator's word and for the marks on §4's items. A `[source-read]` claim
with a `path:line` was read at `8274c951`; a claim taken from
`known-gaps.md` carries that entry's own verification commit and is not
re-verified here.

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

### Written 2026-11-26

On `known-gaps-milestone-orders` from `8274c951`, on the operator's word.
No stage is opened and no code is written. **Not reviewed**: the
`critics-developer` review was started and stopped before it reported,
so no line reference or reading in this order has a second reader behind
it.
