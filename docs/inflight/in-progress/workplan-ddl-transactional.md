# Workplan — transactional DDL

Spec: `docs/spec/ddl-transactional.md`. Read §1's table before touching
anything here: this milestone builds **atomicity, isolation and
consistency**, and defers **durability** by name.

Numbering is `DT<n>`. Cite the file, never the bare number — `DT1` also
exists in `docs/workplan-drop-table.md`.

## Where to pick this up

**The milestone is complete (2026-08-15/16): DT1-DT7, and v1's full
scope — `CREATE TABLE`, `DROP TABLE`, `CREATE INDEX`, `DROP INDEX`.**
DT8 (durability) was never scheduled and stays deferred by name.
**DT9 landed 2026-08-18** and took the one decision §5a left open, which
withdrew the `DROP INDEX` refusal described two paragraphs below — read
that history first, then §5b, and note that the refusal text there is
what the engine did between 2026-08-16 and 2026-08-18, not what it does
now.

The index pair landed last and taught the milestone's sharpest lesson —
**by being wrong first.** It shipped claiming `DROP INDEX` is isolated
where `DROP TABLE` is not, and that this proved §5a's limit belongs to
the `sys.objects` retype. Review disproved it: `SHOW INDEXES` filters,
`InitTableAccess` does not, so index maintenance saw an uncommitted drop
immediately and a rollback left an index silently missing rows — a wrong
query result. `DROP INDEX` inside a transaction was refused for it (and
un-refused by DT9, which fixed the read instead), and §5a's real
statement is that **any catalog change unfiltered readers act on cannot
be isolated** — DT9 does not retract that, it makes the unfiltered read
one that no longer acts on an open mark. The lesson is about generalising from one
surface without checking the others. And the
test asserting that isolation failed at first for a reason worth
keeping: `SHOW INDEXES` had been classified as a *diagnostic* alongside
`SHOW ACCESS` and `SHOW BUDGET`. It is not. A surface reporting **schema
objects** is a resolution route and must filter; one reporting **engine
state** is a diagnostic and must not. That misclassification let an
uncommitted `DROP INDEX` be visible to everyone while the rest of the
catalog hid it, and only an adversarial test found it.

**Spec §5's scope line was amended 2026-08-16 to match what shipped**: it
had named `CREATE INDEX` / `DROP INDEX` in v1 and they were not built.
No surface ever claimed them, so nothing overclaimed — but a scope list
that runs ahead of the code is how a later reader concludes a statement
is transactional and learns otherwise in production. Adopting the
mechanism for the remaining DDL is now mechanical; the one to check
first is an index *drop*, since anything that retires rather than
delete-marks inherits §5a's isolation limit and DT5's terminating-sweep
trap. **Spec §1's properties A, B and
C are delivered at the SQL surface**: a rolled-back `CREATE TABLE`
leaves no relation, and an uncommitted one is invisible to every other
session by every route into it. **D (durability) remains deferred by
name** — catalog writes are still unlogged and unrecovered, so nothing
may claim crash-durable DDL. What remains is DT4-DT7 plus the
resolution sites DT3c did not thread (listed there).

## The phases

### DT1 — the spec, and the reversal recorded ✅ 2026-08-15

`docs/spec/ddl-transactional.md`, plus amendments to `docs/spec/txn.md` §7 and
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

**The remaining sites were closed out 2026-08-16**, and closing them
turned out to be three decisions rather than bookkeeping — the
classification now lives in spec §5:

- **Filtered**, joining SELECT/INSERT/DESCRIBE: `UPDATE`, `DELETE`,
  `ALTER`, `DROP TABLE`, `SHOW TABLES`, and a foreign key's parent
  lookup. All decide what a statement may touch.
- **Unfiltered on purpose — the duplicate-name check** in both `CREATE
  TABLE` forms. Filtering it would let two transactions each create a
  relation of the same name; unfiltered, the second is refused. That is
  the conservative half of §6's open decision, now pinned by a test so a
  later change cannot flip it silently.
- **Unfiltered on purpose — diagnostics**: `SHOW ACCESS`, `SHOW BUDGET`,
  `SHOW INDEXES`, `SHOW ASSERTIONS`, `SHOW CABINS`, the name renderer,
  and `ALTER`'s system-relation guard. These answer "what does this
  instance hold", an operator's question; an operator debugging a stuck
  transaction needs to see what it is consuming.

One test now walks **every** route into a relation and asserts they
agree, which is the property that matters: a single route answering
differently is the leak, and that is exactly how `SHOW TABLES` was
missed at DT3c.

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
untouched and still recorded in `docs/inflight/known-gaps.md`. Nothing here keys
on that counter.

### DT5 — DROP, and the delete-mark ✅ 2026-08-16 (option (b))

Decided by the user from three options: **(b) delete-mark plus trail
compensation, no undo records.** A drop inside a transaction
delete-marks its dependents instead of retiring them and captures the
`sys.objects` retype's before-image, both on the trail, so `Abort`
restores the relation and its rows. Autocommit still retires.

**Atomic, not isolated** — spec §5a states what that costs and why: an
in-place overwrite with no undo chain leaves the prior image only in the
aborting transaction's own trail, so other sessions see the drop before
it commits. Closing that is option (a)'s undo records, not built.

Two things this found:

- **The sweep loop stopped terminating.** It runs until nothing matches,
  which a retired slot satisfies by vanishing and a delete-marked one
  does not — so it re-marked the same row forever. Caught as a hang
  rather than a failure. The original *"retired, not delete-marked"*
  comment was load-bearing for termination as well as for reads, and
  changing one half without the other is what broke it.
- **`ForFirstRow` had to report the page it acted on.** The sweeps knew
  the slot but not the page, which is wrong the moment a catalog
  relation overflows onto a chained page — the trail would have
  addressed the wrong row. An optional out-param, so none of its
  fourteen callers changed.

**One pre-existing test asserted the opposite and was inverted, not
deleted**: `DropTableTest.ADropInsideATransactionIsNotRolledBack` pinned
the old limitation, and `docs/spec/drop-table.md`'s own DT5 stated it in
prose. Both are amended, and both now point here. Note the numbering
collision CLAUDE.md warns about is live: **two specs have a "DT5" and
they say opposite things about the same statement** — cite the file.

Gate: **met.** A rolled-back drop restores the relation *and* its rows
(the data pages were never touched); a committed one stays dropped with
its name freed by the tombstone retype; an autocommit drop still retires
and is unaffected by a later unrelated rollback.

### DT5 — original plan (superseded by the entry above)

`DROP TABLE` delete-marks its `sys.tables` row under the transaction's id
instead of tombstoning immediately, so a rolled-back DROP leaves the
relation intact. Interacts with `docs/spec/drop-table.md`'s tombstone
rule — read it first; the oid must still never be reissued.

### DT6 — the second DDL statement, and the refusal ✅ 2026-08-16

Delivered by *not* filtering one read. The duplicate-name check in both
`CREATE TABLE` forms resolves unfiltered, so it sees another
transaction's uncommitted relation and refuses the second create with
`EXISTS` — the recommended half of spec §6, reached by leaving a site
alone rather than by adding a mechanism. Pinned by
`ASecondCreateOfTheSameNameIsRefusedWhileTheFirstIsOpen`, and the name
is free again once the first transaction rolls back.

**What stays open is the message, not the behaviour**: the refusal can
be spurious (the first transaction may roll back) and it names a
relation the asker cannot see. Improving that wording, or holding the
second create instead of refusing it, is spec §6's remainder.

### DT7 — the surface tells the truth ✅ 2026-08-16

- `SHOW META` prints `ddl_transactional=create-table-only` **beside**
  `ddl_durable=0`, deliberately adjacent: a reader of the first will
  assume it includes surviving a crash, and the pair has to read as one
  statement.
- `manual/sql/sql.md` says `CREATE TABLE` is transactional, names both
  limits (not crash-durable; a second transaction cannot take the same
  name while the first is open), and says the other DDL is not — with
  the asymmetry explained where `DROP TABLE` and `ALTER` are documented,
  so a reader meets it at the statement rather than in a footnote.
- `docs/inflight/known-gaps.md` records the same split and names what is still
  non-transactional, with the reason drop is harder than create.

### DT3d — every route takes the statement boundary ✅ 2026-08-16

Found by the milestone's review, and it was a **READ COMMITTED
violation**, not only a DDL wrinkle. `ViewFor` reads the transaction's
view, but only routes reaching `SnapshotFor`/`BeginWrite` re-minted it
at the statement boundary — so `DESCRIBE`, `SHOW TABLES`, `SHOW INDEXES`,
`ALTER`, `DROP TABLE` and the FK parent lookup resolved under whatever
view the transaction last happened to hold. A relation committed after
the transaction began was visible to `SELECT` and invisible to
`DESCRIBE`, in the same transaction — which also breaks DT3c's own
stated property that every route agrees.

**The boundary is latched, not taken per handler**, and that is the
decision. Per-handler would move the view *within* a statement wherever
a handler resolves twice — the FK lookup does — and two resolutions in
one statement could then disagree, which is this same bug in a new
place. One latch, reset at the top of `DispatchInner` where a statement
genuinely begins, taken by whichever reader needs a view first;
`SnapshotFor`, `BeginWrite` and `ViewFor` all route through it. A plain
member is the right scope because one statement runs at a time on a
core.

Reachable only while some transaction holds uncommitted DDL, since that
is when `ViewFor` filters at all — which is why the test has a third
session holding DDL open, and why the bug survived the milestone.

Gate: **met**, and the test was verified to fail with the latch removed
(both `DESCRIBE` and `SHOW TABLES` resolve stale). A second test pins
what the fix risked: `StartStatement` is *the* branch separating READ
COMMITTED from REPEATABLE READ, so taking the boundary more often had to
be proven a no-op under RR rather than assumed.

### DT8 — durability (deferred, not scheduled)

WAL-logged catalog writes and catalog recovery — RV3. Spec §7. Listed so
the milestone's shape is honest, not because it is next.

### DT9 — the unfiltered read learns what an open delete-mark means ✅ 2026-08-18

Spec §5b. Takes §5a's open decision: **an object exists from the moment
its row is written until its removal commits.** One arm of `ScanAll` in
`src/catalog/catalog.cpp` — the only reader of a catalog delete-mark in
the tree — now asks `txn::TransactionManager::IsInFlight` before it
treats a mark as final, and `DROP INDEX` inside an explicit transaction
is allowed again.

**Three things this step is, that a summary of it would lose.**

*The rule is asymmetric and the symmetric one is a bug.* Hiding
uncommitted *inserts* from unfiltered readers as well — the obvious
"just mint a committed-now view for internal reads" — would stop a
session's own uncommitted `CREATE INDEX` from being maintained by its
own `INSERT`s. That is the same wrong-result class, mirrored. Both
halves must fail toward *"the object is there"*.

*The soundness rests on an ordering fact, not a definition.* Reading
"no longer in flight" as "committed" is only safe because
`TransactionManager::Abort` compensates the whole trail **before** it
clears `active_`. Invert that order and this rule breaks silently.

*The scope of the claim is core 0.* `IsInFlight` walks one core's live
list. Sound while CC3 refuses cross-core writes and core 0 alone
listens; it is now §6's newest open item rather than an assumption
buried in a predicate.

**Where it is armed.** `Catalog::SetTransactionManager`, called from the
`CommandDispatcher` constructor rather than from each server's startup —
the one place a catalog and a manager are known to belong together, so a
new construction site (a test fixture especially) cannot silently keep
the pre-DT9 answer. Null leaves every reader as it was, which is what
bootstrap, recovery and a test over a bare store need.

**The bug this step introduced and caught in its own review.**
`EndDdlScope` invalidated the catalog cache on rollback only, because a
commit used not to change what a cached fact meant. It does now. The
window: another session's `INSERT` during an open `DROP INDEX` fills the
cache with the index still in it (deliberately — that is what keeps
maintenance correct under a rollback), and nothing dropped that cache at
commit. Invalidation is now unconditional on a DDL-holding transaction
resolving, and `rows_were_retired` lost its last caller. Pinned by a test
that asserts the version moves across the commit, verified to fail with
the commit-side call removed.

**Also corrected here**: §5a claimed this fix "would let both drops
isolate". It does not. `DROP TABLE`'s exposure is the `sys.objects`
in-place retype, which a filtered read skips outright; no delete-mark
rule reaches an overwrite. That correction is in §5b.

Gate: **met.** The test that asserted the refusal is replaced by one
that runs the wrong-result scenario forwards — session A drops the index
inside a transaction, session B inserts in the window, A rolls back, and
the row must be reachable through the restored index — and it was
verified to fail (0 rows found) with the rule disabled, which is the
only thing that makes it a test rather than a passing assertion. A
second test pins the committed half: once the drop commits, the mark
counts and the name is free again.

### DT10 — delete-marks are finalized at mount ✅ 2026-08-18

Spec §5c. Found by the `critics-developer` pass over DT9's own diff, and
it is the finding that made the review worth running: DT9's rule is
correct within a mount and has exactly one hole across one.

**The hole.** A mark outlives the transaction that wrote it, and the next
mount has no deleter to ask about. `txn/trx_id.hpp` makes it worse than
"unknown": the id ceiling is unlogged, so a crash reissues the block, and
a live transaction wearing a committed dropper's id makes `IsInFlight`
answer true — the dropped index is re-armed by `InitTableAccess` and
probes answer from a btree missing every row written since the drop.
Silently missing rows, in a case the *pre*-DT9 rule got right. No cheap
guard exists: a reissued id sits at or above this mount's floor, exactly
where a live one does.

**Why retiring is the only answer rather than the safe one.** A mark
whose transaction committed should be gone. A mark whose transaction did
not commit cannot be rolled back either — the trail died with the process
and the catalog is not recovered (RV3). Both already read as gone to
every unfiltered reader. The sweep removes the ambiguity, it does not
choose a side.

**The second reason, which would justify it alone**: nothing else purges
these rows. A transactional `DROP TABLE` leaves one mark per column,
index and foreign key, forever, re-read on every catalog cache miss.

**Placement is load-bearing.** After recovery, so a mark this mount's own
log restored is included; before the transaction stack exists, so no live
transaction can own a mark it retires. "Retire every mark" is safe there
and catastrophic anywhere else — that is why the accessor's comment says
so and why it takes no arguments to narrow it. Core 0 only (P6).

Gate: **met.** Two tests — a committed transactional drop leaves a mark
the sweep retires, and the second sweep finds nothing (idempotence is
what keeps an ordinary restart free) — plus a clean instance where it
finds nothing and disturbs nothing. `SHOW META` gains
`catalog_marks_finalized`, and `ddl_transactional` regains `drop-index`,
which is the reason that field is a list of statement names and not a
bare `=1`.

## Rules this milestone inherits

- Every step gets a `critics-developer` review and a `ck-tester` run
  (CLAUDE.md's Session Workflow). DT3 especially: it changes what a read
  returns, which is the class of change that breaks things quietly.
- Ids are burned on rollback, never reused (spec §3). Any test asserting
  "the oid is reused after ROLLBACK" is asserting a bug.
- Bootstrap rows keep `kBootstrapXid`. A test that boots an instance and
  reads a well-known type under a fresh snapshot is the cheapest guard
  against getting DT2 subtly wrong.
