# Work order DW - what a btree relation's deletes leave, purged unasked: index entries first, the row's own space on the operator's word

Status: **WRITTEN 2026-11-26, not opened.** Written on worktree
`known-gaps-milestone-orders` (branch
`worktree-known-gaps-milestone-orders`) from `8274c951`
(`v3.4.17-148-g8274c951e`) on the operator's word of 2026-11-26
(`raft-marks-2026-11-26.md` §3). No stage is started; DW-S0 waits for the
operator's word and for the marks on §4's items. A `[source-read]` claim
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
