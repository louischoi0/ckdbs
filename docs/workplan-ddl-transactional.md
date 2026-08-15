# Workplan — transactional DDL

Spec: `docs/spec-ddl-transactional.md`. Read §1's table before touching
anything here: this milestone builds **atomicity, isolation and
consistency**, and defers **durability** by name.

Numbering is `DT<n>`. Cite the file, never the bare number — `DT1` also
exists in `docs/workplan-drop-table.md`.

## Where to pick this up

**DT1 and DT2 done (2026-08-15).** DT3 is next, and it is the phase
where behaviour actually changes.

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

### DT3 — catalog reads apply the visibility predicate

The walks (`ScanAll` and its single-row sibling) take a
`const txn::Snapshot*` and skip rows the reader cannot see, through the
same `txn::Classify` user reads use. Null means "see everything", which
is what bootstrap, recovery and every internal read take — so the diff is
confined to callers that genuinely have a reader.

This is the phase with real surface: ~16 walk sites inside
`src/catalog/catalog.cpp` and ~31 public read methods, called from ~19
files. Expect the signature churn to dominate the diff and the *decisions*
to be few.

Gate: a test proving one session cannot see another's uncommitted
`CREATE TABLE`, and that `ROLLBACK` leaves no relation — which is
atomicity falling out of isolation, per spec §2.

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
