# Workplan — transactional DDL

Spec: `docs/spec-ddl-transactional.md`. Read §1's table before touching
anything here: this milestone builds **atomicity, isolation and
consistency**, and defers **durability** by name.

Numbering is `DT<n>`. Cite the file, never the bare number — `DT1` also
exists in `docs/workplan-drop-table.md`.

## Where to pick this up

**DT1 through DT4 done (2026-08-15/16).** **Spec §1's properties A, B and
C are delivered at the SQL surface**: a rolled-back `CREATE TABLE`
leaves no relation, and an uncommitted one is invisible to every other
session by every route into it. **D (durability) remains deferred by
name** — catalog writes are still unlogged and unrecovered, so nothing
may claim crash-durable DDL. What remains is DT4-DT7 plus the
resolution sites DT3c did not thread (listed there).

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

### DT3b — a SQL statement's DDL joins its transaction ✅ 2026-08-15

Both `CREATE TABLE` handlers take the `Session`. `DdlScopeFor()` answers
the id a catalog row should carry and where to collect what was written -
`kBootstrapXid` and a null sink outside an explicit transaction, which is
what keeps autocommit byte-identical - and `NoteDdlRows()` registers them
on the trail. **Registration happens before the create's status is
read**, because a create that failed partway still left rows on the page
and those are exactly the rows a rollback must retire.

`CatalogRowRef` also gained `rel_oid` (`kSysObjectsTable` /
`kSysTablesTable` / `kSysColumnsTable`), so if `Compensate` ever does
consult the `RowLocator` it is handed a real relation rather than a zero
nobody can look up. The catalog knows which page it wrote; the caller
would have had to guess.

Gate: **met.** Three tests on the two-sessions-one-dispatcher fixture:
a rolled-back `CREATE TABLE` is gone (and its *name is free again*, which
is what a half-failed migration actually needs); a committed one survives
with usable rows; and an autocommit `CREATE TABLE` is **not** undone by a
later unrelated rollback - the guard against over-registering.

### DT3c — statement resolution passes the session's view ✅ 2026-08-15

`ViewFor(session)` answers the view a statement resolves under, and
**spec §6's cache decision was taken here**: a view is minted *only
while some transaction holds uncommitted DDL* (`ddl_txns_`, entered on
the first catalog row written and left at commit or rollback). With none
in flight every catalog row is bootstrap or committed, so unfiltered is
correct for everyone and the cache fast path is untouched — isolation is
paid for only where isolation is at stake. Inside a transaction the view
is that transaction's own; in autocommit it is minted fresh, which only
happens while DDL is genuinely open.

Threaded into the three routes a relation is reached by: `exec::Compile`
(and `CompileBlock`'s two sub-chain recursions, plus `CompileWhere`,
since a subquery resolves relations of its own), `HandleDescribe`, and
`InsertParsed`.

Gate: **met.** A second session — transactional *and* autocommit — is
refused by `DESCRIBE`, `SELECT` and `INSERT` against a relation whose
creator has not committed, while the creator does all three; and after
`COMMIT` everybody sees it. A second test pins the fast path: the same
statements answer identically before, during and after an unrelated DDL
transaction.

**Resolution sites deliberately left unthreaded**, because they are
DDL/admin paths rather than the routes a relation is *used* by, and each
would need its own reasoning: `HandleAlter`, `HandleDropTable`,
`HandleShowRelayout`, `HandleCreateTableSql`'s duplicate-name check and
its foreign-key parent lookup, the legacy `HandleCreateTable`, and two
`SHOW`-family sites. They see everything, as they did before. Threading
them is bookkeeping, not design — but it is unfinished bookkeeping and
is named here rather than left to be discovered.

### DT4 — the cache honours it ✅ 2026-08-16

The bypass and its gating decision landed in DT3/DT3c. What was left was
a hole that decision *opened*, found by writing the test first and
watching it fail: **a rollback retires catalog rows through the
transaction manager's compensation, so the catalog is never told.** Any
fact cached by an unfiltered read while the DDL was open therefore
outlives the rows it describes — and once the transaction resolves,
`ViewFor` returns to the fast path and serves it. Reproduced with `SHOW
TABLES`, which listed a rolled-back relation from cache.

Two fixes, because the reproduction needed two things to go wrong:

- `Catalog::InvalidateAfterCompensation()`, called from `EndDdlScope`
  **on rollback only, and only for a transaction that actually wrote
  catalog rows**. A commit leaves the rows in place, so what was cached
  about them stays true; a rollback does not. Unlike
  `InvalidateFromPeer` this *does* bump the version — the rows really did
  change on this instance, and a bound statement compiled against a
  relation that just vanished must not read as current.
- `SHOW TABLES` resolves under the session's view like the other three
  routes. It answers "what relations exist", so it must answer it the
  same way `DESCRIBE` and `SELECT` do.

The hazard this phase inherited — `catalog_version()` is not a sound
freshness guard because `InvalidateFromPeer` clears without bumping — is
untouched and still recorded in `docs/known-gaps.md`. Nothing here keys
on that counter.

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
