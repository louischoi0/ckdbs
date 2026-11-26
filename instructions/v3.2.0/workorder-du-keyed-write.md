# Work order DU - a write that finds its rows by a key: a bound on the tuple key, an index, and what a subquery leaves

Status: **WRITTEN 2026-11-26, not opened.** Written on worktree
`known-gaps-milestone-orders` (branch
`worktree-known-gaps-milestone-orders`) from `8274c951`
(`v3.4.17-148-g8274c951e`) on the operator's word of 2026-11-26
(`raft-marks-2026-11-26.md` §3). No stage is started; DU-S0 waits for the
operator's word and for the marks on §4's items. A `[source-read]` claim
with a `path:line` was read at `8274c951`; a claim taken from
`known-gaps.md` carries that entry's own verification commit and is not
re-verified here.

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

| | item | options | CLA proposes |
|---|---|---|---|
| DU-Q0 | Is DU opened, and with what scope? | (a) §2's bounded walk and index-driven write; (b) the bounded walk only; (c) both and a Cabin-driven write | (a): a Cabin set is authoritative only for values observed, and a write that observes nothing gains little |
| DU-Q1 | Does DU-S0 gate? | (a) a premise with a bar: an `UPDATE` by a non-unique index key at 1 M rows measured against its walk; under a 5x saving DU stops at the bounded walk. (b) built regardless | (a) |
| DU-Q2 | The collection's size | (a) bounded by `max_locks_per_txn`, as a walked write's row units are; past it the statement is refused as today. (b) falls back to the walk | (a) |
| DU-Q3 | Where the walk declares the whole key space | (a) the index-driven write declares the same and saves reads only; (b) an index-key range lock is designed - a new unit kind | (a); (b) is a lock-model change for its own order |
| DU-Q4 | Plans | (a) the ladder's rank, and an administrator's one-relation plan may pin a write's arm as it does a read's; (b) rank only | (a) |
| DU-Q5 | A write whose `WHERE` holds a subquery | (a) stays outside DU until the pending entry is ruled on; (b) the operator rules now and DU carries it | (a) |
| DU-Q6 | `PURGE` by list or range | (a) outside DU; (b) inside | (a) |

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
