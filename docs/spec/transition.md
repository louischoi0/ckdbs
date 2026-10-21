# Transition — a column's allowed state changes as a row-local constraint

Status: **built and enforcing** (BV, v3.2.0,
`instructions/v3.2.0/workorder-bv-transition-constraints.md`; concept:
`docs/conceptnotes/cn4-transition-constraints.md`, CN-4, now history).
Depends on: `docs/rules/keystoneid-invariant.md` K1/K2 and its `PURGE`
exception, the dispatcher's write paths (`InsertOneRow`, `UpdateInner`,
`DeleteInner`), the catalog memo and schema word (`docs/spec/catalog.md`),
first-updater-wins (`docs/spec/txn.md` §5).

## 1. What a Transition is

A **Transition** governs one column of one relation, the **state column**.
It declares:

- the **initial set**: the states an `INSERT` may write (`ON INSERT`);
- the **edges**: the `old => new` moves an `UPDATE` may make (`ON UPDATE`);
- optionally, the **deletable set**: the states a row may be deleted from
  (`ON DELETE FROM`). Absent, every delete is allowed.

The **state set** is every value the declaration names. It is closed: a
value outside it is a violation on any event. A **terminal state** has no
outgoing edge; a row there keeps its state for the rest of its life.

```
CREATE TRANSITION order_status ON orders(status)
  ON INSERT ('NEW')
  ON UPDATE ('NEW' => 'CHECKED', 'CHECKED' => 'FINALIZED',
             'NEW' => 'CANCELLED', 'CHECKED' => 'CANCELLED')
  ON DELETE FROM ('NEW', 'CANCELLED');
DROP TRANSITION order_status;
SHOW TRANSITIONS;
```

## 2. Why a path property needs no history

"FINALIZED only after CHECKED" is a property of the path a row's value
takes. It is never declared and never checked on its own. Every `INSERT`
writes an initial state and every `UPDATE` follows an edge, so by induction
every committed value a row holds lies on a walk of the edge graph from an
initial state. The path property follows from the graph: every edge into
`FINALIZED` leaves `CHECKED`. That is why `ON INSERT` and `ON UPDATE` are
both required.

- **Rollback is not a transition.** An aborted `UPDATE` or `DELETE`, live
  or in recovery's undo, returns to a committed value. The undone version
  never joined committed history, so nothing checks it.
- **The induction has one way out: restarting a row's history under its
  old identity.** That means deleting a row, then inserting its tuple key
  again in an initial state. Three facts close it:
  - an issued tuple key is never reused;
  - a deleted key stays bound until a `PURGE` frees it;
  - **`PURGE` is refused on a relation carrying a Transition** (§5).

  A key on a governed relation is therefore bound for life.
- **History is never verified.** `CREATE TRANSITION` accepts a row already
  in a late state if that state is in the state set. A row written while no
  Transition stood, or while one with a different graph stood, is accepted
  too. The graph speaks only for values committed while it stands.

## 3. Rules

**T-R1 — row-local and stateless.** A Transition is its `sys.transitions`
row and nothing else. It has:

- no registry and no derived structure;
- no WAL record kind and no cross-core message;
- nothing for recovery to rebuild.

A check reads the row being written and `TableAccess::transitions`.

**T-R2 — every write path checks.** The checks run on:

- every `INSERT`: textual, multi-row, and the KWP load stream through
  `ExecuteInsert`;
- every `UPDATE` whose `SET` assigns the state column, by literal or by
  expression;
- every `DELETE`.

No bulk path places rows outside the per-row one (`bulkinsert.md` §8). A
write path added later checks, or it refuses the
write on a governed relation. It never skips the check `[quiet-wrong]`.

**T-R3 — a check refuses before anything of its row is written.**

| Statement | Where the check runs |
|---|---|
| `INSERT` | After the foreign-key forward check, before the tuple key is drawn. A refused row burns no id. |
| `UPDATE` | After the row's conflict check, before the assertion reservation, the undo record and the no-refuse window. The old value is the head the borrow holds. |
| `DELETE` | After the conflict check, before the foreign-key reverse check, which may wait. |

**T-R4 — the fence is the relation `X` and the memo `[quiet-wrong]`.**
`CREATE TRANSITION` runs in these steps:

1. Take the relation `X`, as a transaction of the statement's own, as
   `CREATE ASSERTION`'s build does.
2. Make the catalog memo current (`Catalog::Revalidate`) and resolve the
   table name again under a settled view; a name that now names another
   relation is refused `TxnConflict`.
3. The head (`exec::TransitionBuild::ForCreate`): the column, CN-4 T6's
   one-per-column check, the name and the compiled graph.
4. Walk the relation's leaves in slices of
   `exec::SlicePolicy::pages_per_slice` (64), checking every live row's
   state (A1 stage 2, `sched.md` §3). Where the statement can park and
   holds the `X` (`DispatchAsync`), the walk runs on after the head returns
   and yields its core between slices (`server::TransitionBuildRun`), the
   `X` held across every yield; otherwise it runs whole. Each slice reads
   under a check view minted for it - latest settled state - since no view
   is held across a park; under the `X` a later view can only see decided
   what an earlier one saw in flight.
5. Ask the name again - one taken while the build yielded is refused
   `AlreadyExists` - then write the row.
6. Move the schema word (`Catalog::NoteDeclarationsChanged`).
7. Sync (`AwaitDdlDurability`).
8. Release.

`DROP TRANSITION` takes the `X`, makes the memo current, asks the name
again (a Transition moved to another relation meanwhile is refused
`TxnConflict`), retires the row, moves the word, syncs and releases; it
walks nothing and never yields.

The run owns the holder (`DdlBuildLock`), so a build handed on keeps the
`X` until its tail has written, moved the word and synced; a run destroyed
before its tail - its frame destroyed at a suspension - releases it as the
statement's end would, having written nothing. A cancel is taken before
the first slice and at each slice boundary (`protocol.md` §10).

A writer that resolved the relation before step 6 meets the moved word at
its first relation intention (`MemoIsCurrent`). It is refused to run
again, and never writes under the memo that lacked the change. The cell is
`TransitionMemoFenceTest`, and the mutant that removes the memo check fails
it.

**T-R5 — nothing is reserved.** `TRANSITION`, `INSERT`, `UPDATE`,
`DELETE` and `FROM` are matched by text. `=>` is `=` immediately followed
by `>`: two adjacent tokens, so the lexer and the fingerprint do not move.

## 4. Semantics

- **T1 — when the edge is checked: per statement**, as assertions are.
  `NEW => CHECKED => FINALIZED` inside one transaction passes, because each
  step is an edge.
- **T2 — self-loop and untouched column.** An `UPDATE` that assigns the
  current value is no transition and passes. An `UPDATE` whose `SET` does
  not assign the state column is not checked.
- **T3 — `NULL`.** A nullable column cannot carry a Transition.
- **T4 — the state set is closed.**
- **T5 — column types.** The signed integers, `char(N)` and
  `varchar(N)`.
  - A string state compares byte-wise over the unpadded value, as the
    column does (`types.md` §2b). A spilled value compares by content.
  - An integer state compares by value: `'007'` and `7` are one state.
  - A `char(N)` state longer than `N` is refused at `CREATE`.
- **T6 — one Transition per column.**
- **T7 — `DROP TABLE` takes the relation's Transitions.** A Transition's
  row commits at its own statement. In autocommit the drop retires the
  relation's Transitions once it has committed. Inside an explicit
  transaction, a drop of a governed relation is refused `NotImplemented`,
  because a rollback could not give the constraint back.
- **T8 — every write path** (T-R2).
- **T9 — a CN-3 successor's state.** CN-3 is not built. When it is, a
  successor is an `INSERT` and meets the initial set. CN-3's own work order
  may ask to relax that, against BV's tests.
- **T10 — `ON INSERT` and `ON UPDATE` are required.** `ON DELETE FROM` is
  optional, and permissive when omitted.

## 5. `PURGE`

`PURGE` on a relation carrying a Transition is refused `Unsupported` at the
relation's byte, naming the Transition. A freed key could be inserted again
in an initial state, which is the one way past §2's induction. Nothing
records that a key was purged: the slot is retired keyless. So the
re-insert cannot be refused without new durable state, which T-R1 rules
out. `DROP TRANSITION` lifts the refusal.

## 6. Storage

`sys.transitions` is oid 146, with column oids 147..152 and a fixed root
on page 16, the highest of the catalog's fixed roots (`kCatalogRoots`);
past it the chain grows from the general supply (`crosscore.md` CC12
CR3).

- **One of the two declaration relations.** `sys.assertions` is the other:
  same shape, same reader (`exec/declaration_relation.hpp`). A row holds
  the `CREATE TRANSITION` statement verbatim, the relation's oid and the
  governed column's position.
- **The reader re-parses the text.** The catalog's transition loader
  (`Catalog::SetTransitionLoader`, armed by every `CommandDispatcher`)
  re-parses it into `TableAccess::transitions` at the memo's fill. A fill
  that cannot read the relation fails, as one that cannot read `sys.fkeys`
  does.
- **`ALTER TABLE RENAME` moves nothing.** The column is held by position
  and the text is read only for its states.
- **A dropped Transition's spilled text** is collected by the mount's
  var-heap sweep (`catalog_spills.hpp`).
- **`transitions` is a reserved relation name**, as every bootstrap
  relation's name is: `CREATE TABLE transitions` is `AlreadyExists`.

## 7. Refusals

- **A write** is `TRANSITION_VIOLATION`: `StatusCode::kTransitionViolation`,
  wire category 18, `retryable=0`. Its message names the Transition, the
  column, the state or edge, the row id where one exists, and the value's
  byte where the value was written in the statement. Like every write
  failure it fails the transaction, because there is no statement-level
  rollback.
- **A declaration** is refused with its byte for any of these:
  - an unknown relation, a system relation, or an unknown column;
  - the tuple key column;
  - a nullable column, or a column of another type;
  - a missing `ON INSERT` or `ON UPDATE`;
  - a malformed `=>`;
  - a literal of the wrong kind, or one longer than `char(N)`;
  - a state no walk from an initial state reaches;
  - a second Transition on the column, or a taken name;
  - an existing row outside the state set (`TRANSITION_VIOLATION`);
  - a relation with an in-flight write (`TXN_CONFLICT`, retryable);
  - a cancel taken at a slice boundary of the build (`CANCELLED`, T-R4).
    Nothing is written, and an open explicit transaction is left failed
    (the operator's ruling of 2026-10-21, `ddl-transactional.md` §5f); a
    declaration's other refusals poison nothing.
