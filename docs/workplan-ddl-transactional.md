# Workplan — transactional DDL

Spec: `docs/spec-ddl-transactional.md`. Read §1's table before touching
anything here: this milestone builds **atomicity, isolation and
consistency**, and defers **durability** by name.

Numbering is `DT<n>`. Cite the file, never the bare number — `DT1` also
exists in `docs/workplan-drop-table.md`.

## Where to pick this up

**DT1, DT2, DT3 and DT3a done (2026-08-15).** The catalog mechanism is
complete: isolation by view, atomicity by compensation. What is left is
**wiring it to a session** — nothing above the catalog passes a real
transaction id or a view yet, so no SQL statement is transactional DDL
today. That is DT3b, and it is next.

## The phases

### DT1 — the spec, and the reversal recorded ✅ 2026-08-15

`docs/spec-ddl-transactional.md`, plus amendments to `docs/txn.md` §7 and
§9 so the docs stop saying "out of scope" while the code does it. No code.

### DT2 — a catalog row can carry a real transaction id ✅ 2026-08-15

`InsertRow()` already took an id; what hard-coded `kBootstrapXid` were
its callers. `CreateTable` now takes a `trx_id` (defaulted) and threads
it to all three of a relation's rows - `sys.objects`, `sys.tables`,
`sys.columns` - deliberately the *same* id for all three, because a
reader that could see the table row but not its columns would see a
relation with no schema, which is worse than not seeing it at all. Bootstrap paths keep
`kBootstrapXid` explicitly (spec §3 — the well-known rows must stay
visible to a view minted before any transaction existed).

**Nothing changes behaviourally in this step**: the dispatcher still
passes `kBootstrapXid` for user DDL, because reads do not filter yet, and
a real id with unfiltered reads would be *less* correct, not more (a row
would be visible to everyone including its own aborted transaction).
DT2 is the seam, and it is separated from DT3 precisely so the
behavioural change lands in one reviewable step.

Gate: **met** - 2,363/2,363 unchanged, plus two new seam tests
(2,365 total). One asserts a supplied id reaches all three pages and was
verified to fail when the id is dropped on any one of them; the other
asserts every row still carries `kBootstrapXid` when no id is passed,
which is the half that would have broken quietly.

### DT3 — catalog reads apply the visibility predicate ✅ 2026-08-15

`ScanAll` takes an optional `const txn::ReadView*` and skips rows the
reader cannot see; null means "see everything" and reproduces every
pre-DT3 caller byte for byte. `FindTableOidByName` and `ListTables` take
one and pass it down.

**Scoped to the name lookup, deliberately.** SQL reaches a relation by
name and never by oid, so a name that does not resolve is a relation that
cannot be touched — which is why `InitTableAccess` stays cache-served and
unfiltered. It is reachable only with an oid the caller could only have
got from a lookup that already applied the filter.

**A transactional lookup neither reads nor fills the shared cache**
(spec §4's option (a), scoped tighter than proposed: keyed on "a view was
passed", not on "this session did DDL"). The cache is one map per
instance and knows nothing about who is asking, so filling it from a
filtered read would publish an uncommitted relation to everybody. Both
halves are tested; the cache half is the one that would have broken
quietly.

Gate: **met for isolation** — a reader whose view cannot see the creating
transaction gets `NotFound` and an unlisted relation, while the creator
sees its own work and an internal (null-view) read still sees everything.
**Not met for atomicity, and it cannot be** — see below.

### DT3a — rollback actually removes the relation ✅ 2026-08-15

**DT3 discovered that spec §2 was wrong**: `ReadView::Visible` has no
notion of "aborted", so once the aborting transaction leaves the live
set, a later view reads its id as committed. Visibility delivers
isolation and nothing else.

The engine hides aborted work by **compensation**, not visibility:
`TransactionManager::Abort` walks the trail in reverse and
`RetireSlot`s each insert. So DDL must register its catalog row on the
trail via `NoteInsert`, which needs the `(page_id, slot)` that
`InsertRow` currently discards — that return value is the whole of the
code change.

**The flagged risk resolved favourably and needed no code.**
`Compensate` confirms identity by re-reading the row and comparing
`entry.pk` through `KeystoneIdOfPayload` — a *user row's* Keystone id,
which a catalog row has no equivalent of. It works anyway: every catalog
row carries its `oid` in its first eight bytes, exactly where a Keystone
id sits, so recording `entry.pk = row.oid` makes the check pass **and
still check** (it is a real comparison, not a bypass — the test passes no
`RowLocator`, so a mismatch would have failed the abort rather than
silently relocating). Stated because it is a coincidence of layout that
a future row format could break.

`InsertRow` grew an optional out-param rather than a changed return type:
nine call sites, and only three care.

Gate: **met** — a `CREATE TABLE` under a live transaction, its three rows
registered with `NoteInsert`, then `Abort`, and the relation is gone to a
view minted *after* the rollback (the one that would wrongly have seen
it) **and** to an unfiltered read — which is what proves the rows were
retired rather than merely hidden.

### DT3b — a SQL statement's DDL joins its transaction

Everything above is reachable only from C++. `HandleCreateTableSql` takes
no `Session`, so no statement passes a transaction id or a view, and
`BEGIN; CREATE TABLE t; ROLLBACK` still leaves a relation. This phase is
the plumbing: thread the session into the DDL handlers, pass its
transaction's id to `CreateTable`, register the returned rows on its
trail, and pass its view to relation resolution.

Note the resolution surface is wide — ~14 `FindTableOidByName` callers
outside the catalog, 10 of them in the dispatcher — and each needs to
know whether its caller is inside a transaction. Expect churn to dominate
and decisions to be few, as DT3 did.

Gate: the SQL-level statement of DT3a's property, over a real session.

### DT4 — the cache honours it

Per spec §4 and §6's open decision. **Blocked until that decision is
ratified**; recommendation is (a) bypass — a session with DDL in its open
transaction reads uncached until it resolves.

Inherits a known hazard: `catalog_version()` is not a sound freshness
guard, because `InvalidateFromPeer()` clears the cache without bumping it
(`docs/known-gaps.md`). Any scheme keyed on that counter is wrong on a
peer.

### DT5 — DROP, and the delete-mark

`DROP TABLE` delete-marks its `sys.tables` row under the transaction's id
instead of tombstoning immediately, so a rolled-back DROP leaves the
relation intact. Interacts with `docs/spec-drop-table.md`'s tombstone
rule — read it first; the oid must still never be reissued.

### DT6 — the second DDL statement, and the refusal

Two uncommitted transactions creating the same name (spec §6). The
recommendation is to refuse the second rather than accept last-writer-
wins. Needs DT3, because "is there an uncommitted row with this name"
is a visibility question.

### DT7 — the surface tells the truth

`SHOW META`, `manual/sql/sql.md` and `docs/known-gaps.md` state what is
true: atomic and isolated, **not yet crash-durable**. Owed before any
release note says "transactional DDL", and cheap to forget.

### DT8 — durability (deferred, not scheduled)

WAL-logged catalog writes and catalog recovery — RV3. Spec §7. Listed so
the milestone's shape is honest, not because it is next.

## Rules this milestone inherits

- Every step gets a `critics-developer` review and a `ck-tester` run
  (CLAUDE.md's Session Workflow). DT3 especially: it changes what a read
  returns, which is the class of change that breaks things quietly.
- Ids are burned on rollback, never reused (spec §3). Any test asserting
  "the oid is reused after ROLLBACK" is asserting a bug.
- Bootstrap rows keep `kBootstrapXid`. A test that boots an instance and
  reads a well-known type under a fresh snapshot is the cheapest guard
  against getting DT2 subtly wrong.
