# CN-4 — Concept Note: Transition — a column's allowed state changes as a constraint

Status: **[open]** (operator, 2026-10-14) — taken up by
`instructions/v3.2.0/workorder-bv-transition-constraints.md` (BV, v3.2.0),
which is now the authority; this note is history, and §8's open items are
marked there. As written: CONCEPT, not a work order, not a CIP. Nothing here opens a stage.
**No work starts on this note until the operator rules on §8.** Ordering
relative to CN-1, CN-2 and CN-3 is not fixed.
Author: CLA, 2026-09-23, against `d2e123d`
Re-read: CLA, 2026-10-14, against `b0bc959c`. The body (§0–§8) stands as
written at `d2e123d`; what the tree has moved since is §9, and **§2's
third fact no longer holds** (§9.1).
Origin: operator's verbal concept of 2026-09-23 (§0), elaborated by CLA in
one exchange; the operator has answered none of §8.
Claim tags: `[operator]` for the operator's statements; `[source-read]`
with `path:line` at `d2e123d`; everything else `[design]`. Nothing is
`[measured]`.
Relation to `docs/rules/keystoneid-invariant.md`: this note **relaxes
nothing** and depends on K1 and K2 (§2) — and on one fact that is not an
invariant, that nothing retires a deleted slot (T11). Relation to CN-3: a
successor row is the one place the two must agree (§6, T9).

---

## 0. The concept, in the operator's words

`[operator]` A **Transition** is a constraint on the chain of values one
column may pass through. The example: a status column's initial value is
always `'NEW'`, and it never becomes `'FINALIZED'` without having been
`'CHECKED'` first. It is declared as an enumeration of `old => new` pairs,
and it is settable **on insert, on update and on delete**.

## 1. Definitions

`[design]`

- **State column** — the one column a Transition governs.
- **State set** — every value named anywhere in the declaration (closed
  or not: T4).
- **Initial set** — the states an `INSERT` may write (`ON INSERT`).
- **Edge** — an allowed `old => new` pair (`ON UPDATE`).
- **Deletable set** — the states a row may be deleted from (`ON DELETE`).
- **Terminal state** — a state with no outgoing edge. A row there keeps
  its state column for the rest of its life.

## 2. Why a path property needs no history on this engine

`[design]` The operator's example is about a *path* ("CHECKED before
FINALIZED"), and a path can only be checked by reading history — unless
every step of it is checked. If every `INSERT` writes a state in the
initial set and every `UPDATE` moves along an edge, then by induction
every committed value a row has held lies on a walk of the edge graph
from an initial state. "FINALIZED only after CHECKED" then **follows from
the declared graph** (every edge into `FINALIZED` leaves `CHECKED`); it is
never declared or checked on its own. The induction needs both
`ON INSERT` and `ON UPDATE` to be present (T10).

**Rollback is not a transition.** An aborted `UPDATE` restoring
`'CHECKED'` over `'FINALIZED'`, recovery's undo doing the same, or an
aborted `DELETE` bringing a row back all return to a committed value; the
undone version never joined committed history, so the induction is over
committed versions and none of them is checked.

The induction has one way to be broken: a row that restarts its history
under the same identity — delete it, then insert the same id back in
state `'NEW'`. This engine refuses that today, on three facts
`[source-read]`: the pk is *"never rebound and never updated"*
(`docs/spec/heap-and-tuple.md:109`, K1/K2); an omitted pk is issued from a
sequence that never reuses (`:132`); and a **named** pk equal to a deleted
one is refused because *"a `DELETED` slot is still a live slot until
retirement; nothing retires a slot today"* (`:168`). The third is a
statement about today, not an invariant — a future slot purge reopens the
restart on a btree relation (T11). CN-3's succession is the one designed
way around the same guarantee (§6, T9).

## 3. What a check reads, and where it runs

`[design]` unless marked.

- **Per row, from the row alone.** `INSERT`: the new value against the
  initial set. `UPDATE`: the version being replaced and the new value
  against the edges. `DELETE`: the version being marked against the
  deletable set. No other row, no other relation, no aggregate.
- **No derived state.** Everything a Transition needs is its catalog row.
  There is no structure to build, reserve into, snapshot or recover —
  the whole of what `docs/spec/assertion.md` §5–§7 carries for an
  assertion, and where both of its open defects lived when this note was
  written (`a-chunked-assertion-snapshot-can-be-split-by-another-cores-record.md`,
  fixed at AY-S8 and deleted, and
  `docs/inflight/bugs/assertion-reservations-stranded-by-a-failed-settle.md`).
  No WAL record kind, no cross-core protocol.
- **No new concurrency.** What makes the old value the one being replaced
  is first-updater-wins `[source-read]`: `TransactionManager::CheckWriteConflict`
  (`src/txn/manager.cpp:199`) refuses a write over a head whose writer the
  view cannot see, and the write's borrow — the tuple's `X`, or a declared
  range for a pk-window or unfiltered statement (`docs/spec/txn.md` §5) —
  keeps a second writer out until decide. A writer that waited re-runs and
  reads the other's result under RC, and is refused `TxnConflict` under RR.
  Either way the check never sees a value that is not the head.
- **Where it sits** `[source-read]` — beside the foreign key's per-row
  check, at the sites that already carry it: `InsertOneRow`
  (`src/server/command_dispatcher.cpp:5818`, reached from `InsertParsed`),
  `SortedFillInner` (`:5546`), `UpdateInner`'s per-row lambda for a column
  its `SET` touches (`:8523`), and `DeleteInner`'s mark lambda (`:10077`,
  after its borrow and conflict check). The KWP load path reaches
  `InsertParsed` through `ExecuteInsert` (`:5157`, called from
  `src/server/kwp_load_server.cpp:462`).
- **A violation poisons, like every other write failure**
  (`docs/spec/assertion.md` §4.4, `[source-read]`): the engine has no
  statement-level rollback, so an `UPDATE` failing on row 3 of 10 fails
  the transaction, not the row.

## 4. Surface, and `CREATE` over existing rows

`[design]` A sketch, not a grammar decision.

```
CREATE TRANSITION order_status ON orders(status)
  ON INSERT ('NEW')
  ON UPDATE ('NEW' => 'CHECKED', 'CHECKED' => 'FINALIZED',
             'NEW' => 'CANCELLED', 'CHECKED' => 'CANCELLED')
  ON DELETE FROM ('NEW', 'CANCELLED');

DROP TRANSITION order_status;
SHOW TRANSITIONS;
```

- `TRANSITION` hashes as an identifier; nothing new is reserved
  (`CLAUDE.md` Working Rules). A grammar addition is additive shape and
  moves no `kFingerprintVersion` (`docs/spec/parser-v2.md:90`,
  `[source-read]`) — to be confirmed against the golden corpus, not
  assumed.
- **Violation code**: a new `TRANSITION_VIOLATION`, non-retryable, beside
  `kFkViolation` and `kAssertionViolation`
  (`include/kds/base/status.hpp:87`, `:103`) and appended to the wire's
  pinned list the way those two were (`include/kds/wire/kwp.hpp:258`,
  `[source-read]`). Folding it into `InvalidArgument` would give a client
  nothing to switch on. The message names the Transition, the row id and
  both values.
- **Create-time refusals**, before any scan, in the shape of
  `assertion.md` §3.1: unknown relation or column; the pk column; a type
  outside T5; a nullable column (T3); a missing required clause (T10); a
  duplicate name; a second Transition on the column (T6); a state no walk
  from an initial state reaches.
- **Existing rows.** The build scans the relation and refuses the `CREATE`
  if a row's state column is outside the state set, as `CREATE ASSERTION`
  refuses over an existing violation (`assertion.md` AS7,
  `[source-read]`). **History is never verified**: a row already in
  `'FINALIZED'` that never passed `'CHECKED'` is accepted — and so is
  every row written while no Transition, or a Transition with a different
  graph, stood on the column (`DROP` then `CREATE`). The graph speaks
  only for values committed while it stood.
- **The build's fence.** `[design]` It takes the relation's `X` on the
  session's core, as the index and assertion builds do since AT-S5e
  (`instructions/v3.0.0/workorder-at-m3-uniformity.md:476`,
  `[source-read]`), so no row lands behind the scan. A writer that
  resolved the relation before the `CREATE` is kept from writing under
  its stale memo by its own relation `IX` and C2's memo check — and that
  row records C2 as **uncovered**: its mutant survived 0/1, no cell
  holding a writer between its boundary and its bind. A Transition
  created in that window would be skipped quietly, so a work order owes
  that cell before it relies on the fence.

## 5. Positioning

`[design]` Workflow and ledger states are the shape an OLTP workload
writes most and enforces most often in application code, where a second
writer bypasses it; this is one column, one relation, a row-local check.

## 6. Placement against existing structures

`[design]`

- **Assertions** — a group aggregate over many rows, with a Bound Cabin.
  A Transition is row-local and stateless; it shares the violation shape
  and the build fence, and nothing else.
- **Foreign keys** — a Transition takes the same per-row check sites and
  never reads another relation.
- **CN-3 (supersede and chain)** — `INSERT ... CHAIN ON x` creates a
  successor that stands for `x`. It is an `INSERT`, so it meets the
  initial set; whether it should instead inherit `x`'s state, or be
  refused unless its state follows an edge from `x`'s, is T9. The plain
  `INSERT` rule reopens, under an alias, the restart §2 depends on.
- **Waystone, Cabin, the physical optimizer** — unaffected: none of them
  changes a value, and a relayout move is not an `UPDATE`.
- **`ALTER TABLE RENAME COLUMN`** — the catalog row names the column by
  number, so a rename changes only what `SHOW TRANSITIONS` prints.

## 7. What this note does not license

- Any code, grammar, catalog relation, status code or wire entry.
- A trigger framework, or any action on a transition — setting another
  column, writing another row (the foreign key's F4, for the same reason).
- A stored history of transitions, an audit log, or transition timestamps.
- A state machine spanning columns or relations.
- Guarded transitions (`'CHECKED' => 'FINALIZED' WHEN approver IS NOT NULL`).
- Freezing the rest of a row in a terminal state — a separate concept
  with real demand behind it, not a v1 option.
- A claim about cost: the per-row check has never been measured, and the
  overhead measurement is suspended by operator decision.

## 8. Operator decisions this note leaves open

Each carries CLA's proposal, none ratified.

| # | question | CLA's proposal |
|---|---|---|
| T1 | When is the edge checked: per statement, or old-at-`BEGIN` against new-at-`COMMIT`? | **Per statement**, as assertions are (AS3). `NEW => CHECKED => FINALIZED` inside one transaction passes because each step is an edge |
| T2 | A self-loop, and an `UPDATE` whose `SET` does not touch the column | Untouched: no check, as the foreign key does. `SET status = 'NEW'` on a `'NEW'` row is a self-loop, **implicitly allowed** |
| T3 | `NULL` in the state column | **Refuse a nullable column**; `NOT NULL` is already the default. The alternative makes `NULL` a nameable state |
| T4 | Is the state set closed? | **Yes** — a value outside it, on any event, is a violation |
| T5 | State column types | `char(N)`, `varchar(N)` (compared by content, spilled or not) and integer types |
| T6 | How many Transitions per column | **One**. Several would mean an intersection nobody declared |
| T7 | `DROP TABLE` with a Transition on it | **Drop it with the table** — single-relation, holding nothing another relation needs. Assertions are RESTRICT; the asymmetry is stated rather than inherited |
| T8 | Which write paths | Every one that writes the column: textual and multi-row `INSERT`, the KWP load path, `UPDATE`, `DELETE` — and sorted fill, which since SUS-1 is reachable only for a heap relation created before the suspension. A path that cannot check refuses the write, never skips the check |
| T9 | A CN-3 successor's state | Undecided by CLA; the shapes are in §6. **Answer before either note is built**, because one answer voids §2 under an alias |
| T10 | Which clauses are required | **`ON INSERT` and `ON UPDATE` required**, `ON DELETE` optional and permissive when omitted. Omitting either of the first two gives up the path property of §2 with nothing to tell the user so |
| T11 | §2 leans on "nothing retires a slot today" (`heap-and-tuple.md:168`) | Record it as a dependency: any slot purge that lets a named pk re-take a deleted id must answer for Transitions in the same work order. See §9.1 (re-read of 2026-10-14) |

## 9. Re-read against `b0bc959c` (2026-10-14)

The body above is the note as written at `d2e123d`, and it is not amended
(`docs/conceptnotes/README.md`). This section records what the tree has
moved since. Tags as in the header; `[source-read]` here means at
`b0bc959c`.

### 9.1 §2's third fact is gone: `PURGE` reopens the restart

`[source-read]` §2 said a named pk equal to a deleted one is refused
because *"nothing retires a slot today"*. That stopped being true at
BH-S3 (`26766699`, 2026-10-08). `PURGE FROM t WHERE <pk comparisons>`
retires, keyless, every committed delete-marked row its window names once
no reader can see the delete, *"and an `INSERT` that names it is judged as
if the key had never been placed"* (`docs/spec/heap-and-tuple.md:286`,
§4.1c). K1 now reads *"never rebound - except by an explicit `PURGE`"*
(`:105`, `:359`; `docs/rules/keystoneid-invariant.md:10`).

`[design]` What this does to the note:

- **§2's induction has a hole on every btree relation.** `DELETE` (from a
  deletable state), `PURGE`, then `INSERT` with the same id in `'NEW'`
  restarts the row's history under its old identity. Each step is legal
  under the graph as §3 checks it. The other two of §2's facts still
  hold: issued ids are a high-water mark that never moves back (`:109`,
  `:114`), and a heap refuses a named key below its mark (`:169`).
- **That covers every relation created today.** `PURGE` runs on btree
  relations only (`:288`), and since SUS-1 every new relation is a btree.
  The hole is not limited to a niche path.
- **T11 is triggered and unanswered.** T11 asked that the work order
  building a slot purge answer for Transitions. No Transition existed to
  answer for: `instructions/v3.0.0/workorder-bh-purge-key.md` never names
  CN-4 or a Transition. The question therefore goes to whatever work order
  takes up this note.
- **What the purge cannot reach.** `PURGE` is autocommit-only, admin-role
  only, and frees only a key whose deleter every reader can see
  (`:288`, `:290`). The restart is an administrator's act, not an
  ordinary writer's. That bounds who can break the induction. It does not
  close the hole.

`[design]` Shapes an answer could take. None is proposed here.

- (a) The key's identity is the row's history: a Transition refuses a
  `PURGE` of a key on its relation, or refuses a named `INSERT` of a
  purged key.
- (b) A purged key is a new row: the restart is declared out of scope,
  and §2 is re-worded to cover *one tenure of a key*, not the key.
- (c) `PURGE` is the boundary the operator rules on, alongside T9, which
  reopens the same restart under CN-3's alias.

### 9.2 Citations that moved

`[source-read]` Each `path:line` the body cites at `d2e123d`, at
`b0bc959c`. Unlisted citations hold as written.

| body cites | now |
|---|---|
| `heap-and-tuple.md:109` (K1/K2, never rebound) | `:105`, with `PURGE`'s exception |
| `heap-and-tuple.md:132` (omitted pk never reuses) | `:109`, `:114` (the mark never moves back) |
| `heap-and-tuple.md:168` (nothing retires a slot) | gone; `:175` (a committed key stays bound until a `PURGE` frees it) and §4.1c (`:284`) |
| `src/txn/manager.cpp:199` `CheckWriteConflict` | `:188` |
| `command_dispatcher.cpp:5818` `InsertOneRow` | `:5658` |
| `command_dispatcher.cpp:5546` `SortedFillInner` | `:5432` |
| `command_dispatcher.cpp:8523` `UpdateInner`'s per-row lambda | `UpdateInner` at `:7935`; the per-row conflict check `:8246`, the per-row foreign-key check `:8340` |
| `command_dispatcher.cpp:10077` `DeleteInner`'s mark lambda | `DeleteInner` at `:9909`; the borrow and conflict check `:10063`–`:10067`, the delete-mark `:10155` |
| `command_dispatcher.cpp:5157` `ExecuteInsert` | `:5232` |
| `kwp_load_server.cpp:462` (calls `ExecuteInsert`) | `:513` |
| `status.hpp:87`, `:103` | `:88` `kFkViolation`, `:103` `kAssertionViolation` |
| `kwp.hpp:258` (pinned wire list) | `:247`–`:248` |
| `parser-v2.md:90` (no fingerprint bump for additive grammar) | `:88` |
| `workorder-at-m3-uniformity.md:476` (AT-S5e's build fence) | `:486` |

### 9.3 Other facts the body states that moved

- `[source-read]` §3's two assertion defects are both gone from
  `docs/inflight/bugs/`. The chunked-snapshot entry was fixed at AY-S8, as
  §3 already says. `assertion-reservations-stranded-by-a-failed-settle.md`
  was closed unreproduced at `d83bd502` (2026-10-13), not fixed. The point
  §3 makes stands: a Transition carries none of the derived state either
  defect lived in.
- `[source-read]` §4's C2 remark is AT-S5e's own record (`:486`): *"C2's
  stale-memo check survived its mutant 0/1 - no cell holds a writer
  between its boundary and its bind"*. The check itself is still at
  `command_dispatcher.cpp:9181`. This re-read did not establish whether a
  later stage added the missing cell, so §4's caveat stands as written.
- `[source-read]` §3's "no statement-level rollback" still holds
  (`docs/spec/assertion.md:193`), and so does first-updater-wins
  (`docs/spec/txn.md` §5, `:681`).
