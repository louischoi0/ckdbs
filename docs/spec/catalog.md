# Catalog — the schema version word, and what it is not

Opened 2026-09-09 at AT-S2c (`instructions/v3.0.0/workorder-at-m3-uniformity.md`),
on AR0-5 §2.1's instruction that the order below be *"written in that
order in `catalog.md` so nobody later removes the lock because the word
'covers it'"*. This file owns one thing: how every core's view of the
catalog stays current once no core is told anything. The catalog's rows,
pages and DDL semantics stay where they are — `heap-and-tuple.md` §4,
`ddl-transactional.md`, `alter.md`, `drop-table.md`. **A second thing
since AT-S17**: how a name is taken so that two cores cannot both take it
(CT7), which every one of those DDLs leans on and none of them owned.

## CT1 — The lock is the argument; the word is the fast path

A statement holds a relation `IS` on every relation it binds, taken by
the compiler between the name and the schema (AT-S1). **`DROP TABLE` takes
the relation `X`** (`drop-table.md` DT7), and while a statement's `IS` is
granted the drop waits for the statement to end. `CREATE INDEX`, `DROP INDEX` and a `CREATE ASSERTION`'s build take
it too, since AT-S5e (`ddl-transactional.md` §5e, §5f; a plain
autocommit `CREATE INDEX` takes `IS` instead since CI, `index.md` §10.3), both renames
since the operator's ruling of 2026-10-21, under a holder of the
statement's own (`alter.md` AL4a), and a child's
`CREATE TABLE` takes its parent's `IS` for each foreign key it declares
(BF-Q11 (a)). That is the lock's reach, and `txn.md` §5 owns it: the
foreign-key check
family declares only D9(a)'s parent `IS` and `S` (`foreign-keys.md`).
Under those, as under a refused `IS`, what makes a statement's answer
right is catalog MVCC, the absence of data-moving DDL and - for a dropped
relation - the statement epoch that holds its pages' reclaim until every
statement that could have bound it has ended (`drop-table.md` DT1), never
the word.
**The order this file exists to state**: the lock is the argument where it
reaches, the word is the fast path everywhere, and nobody removes the lock
because the word "covers it".

The **schema version word** saves a re-parse and nothing else. One
`std::atomic<uint64_t>` for the instance, owned by `Expeditor`,
memory-resident and never persisted — every cache is empty at mount, so no
value survives one. Every core's `Catalog` holds a pointer to it
(`Catalog::SetInstanceWords`, CT6). **It is not `Catalog::catalog_version()`**,
which is one core's counter (`catalog.hpp`) that another core's drop does
not advance — so nothing shared may be validated against it — and the two
are not merged:
the word is bumped by every invalidation that can stale another core's
memo. (Two that cannot do not bump it: the mount's post-redo `DropCache()`
and its delete-mark purge.)

## CT2 — Bumped at the write, asked at the boundary

**Bumped at the catalog write**, by `Catalog::BumpVersion()`, the single
DDL choke point, which is before the DDL's relation `X` is released at its
decide (AR0-5 D21 as marked). Both endings of a catalog-writing
transaction move the word again at the decide, still before the `X` is
released (`TransactionManager::MoveSchemaWordIfCatalogWriter`, on commit
and on abort), and bump once more after the release through
`InvalidateAfterCompensation()` — a rollback because the rows were
compensated behind the cache's back, a commit because a delete-mark starts
counting then (`ddl-transactional.md` §5b). A statement whose word is at or
above a DDL's decide move therefore bound the DDL's result; that is what
`PURGE DROPPED COLUMNS`' epoch gate rests on (`alter.md` AL11). The writes that keep their own cached entry across a bump are the
in-place root moves (`AdoptRoot`), because
the `INSERT` doing one holds the relation's `TableAccess*`; each bumps the
word, and other cores re-read the root from the anchor.

**Asked at a task boundary, and inside one only before it holds a
`TableAccess`.** `Catalog::Revalidate()`
is one acquire load; on a mismatch the cache is dropped and the value
adopted. It runs between tasks, at two sites:
`CommandDispatcher::DispatchAndStage`'s head (its `StatementEpochScope`,
which publishes the statement epoch around the read, BF-R9) and the Cabin
optimizer's tick (`CabinOptimizerExecutor::Tick`). **Inside a statement it
runs at three, each right after a relation `X`'s grant - or, for a
rename, where the session's transaction already holds it** -
`HandleTransition`, `HandleAssertion` and `HandleAlter`'s renames (the BV
S2 review's B2; `alter.md` AL4a): a DDL another core published and
released between the statement's boundary and an uncontended grant would
otherwise be missing from what the statement reads under the `X`. Each is
safe because nothing of the statement holds a `TableAccess` before its
grant - what it resolved is an oid - so the drop strands no borrow, and
from the grant on no DDL changes the relation until the statement ends.
**It is never called from a cached read**, because a drop frees every
`const TableAccess*` and `Schema&` a running statement holds
(`catalog_cache.hpp`'s entries are reference-stable *until the next
drop*), and a drop inside a statement is a read through freed memory —
a join binding its second relation, an FK loop over a borrowed
`fkeys` vector. Every public dispatcher entry is therefore a
cache-dropping boundary, and nothing may hold a catalog borrow across one
(`command_dispatcher.hpp`'s contract on `DispatchAndStage`). A borrow
that crosses a *park* is re-taken or copied after it (`step_vm.cpp`'s
re-`Bind`, a producer's schema copy), which is the invariant a boundary
relies on.

A writer's own cache is not dropped by its own bump, and a cache that is
behind the word and bumps it does not adopt the new value — `BumpWord` in
`catalog.cpp` carries the rule and its reason on the branch that decides
it.

## CT3 — What a task that asks nothing serves

A task that reads the catalog without a boundary serves a memo as fresh
as its last boundary. Stale, never wrong, by CT1's three facts.
Where staleness has a cost the task asks: the Cabin optimizer's tick.

## CT4 — Why a drop of the memo is enough

One frame table serves every core (AM-S2 step 3, `page.md` §6): the
re-read after a drop finds the bytes the writer wrote, under the page
latch. Catalog writes are WAL-logged like every other (`ddl-transactional.md`
§7), so no page-level force is owed at a DDL's commit — redo carries the
page; a fixture that opens a second store over the device still flushes,
the engine does not.

## CT5 — What stays special about the catalog's pages, and what does not

Two things, neither an authority (AR0-5 §2, AT-R11): the fixed roots'
frames are pinned - they sit below the resident floor (`eviction.md` EV3),
and a page a chain grows onto past its root is an ordinary evictable frame
- and page 0's address with the fixed root page numbers is bootstrap
layout. Every core writes them, so every core's cache can be
behind the word, and `BumpWord`'s adopt-only-if-current rule (CT2) is live
on every core.

**A catalog row is not a lock unit** (AT-S3, E13 answered no). A named
key's admission writes the relation's `sys.tables` row - the mark -
outside the caller's transaction (`heap-and-tuple.md` §4.1: the advance
outlives a rollback), and that write has no contention problem a lock
would fix. The mark is a monotone number nobody caches: its
read-modify-write is atomic because **no task parks under a page span**
(`page.md` §6's discipline) — the page latch serialises cores, and it is
re-entrant for the owning core, so within one core it is the no-park rule
and nothing else. A transaction-length `X` would
serialise every named-key `INSERT` into a relation for the length of each
transaction and protect nothing. Every core admits a named key, the page
write being every core's. An omitted key's issue (`AllocateRowId`) is the
same read-modify-write of the same row, made by every core with no
per-core cache (`heap-and-tuple.md` §4.1a). `rules.md` §3
declares the pages, not the rows.

**Placing a *new* catalog page is every core's too, since AT-S5b**, and
**it comes from the general supply since superblock 31** (D1 of the
catalog page ceiling, the operator's ruling of 2026-10-18,
"$!recommended"). The
pages CT5 opens with are the fixed roots (`kCatalogRoots`,
`include/kds/catalog/well_known.hpp`); a catalog relation's chain grows
past its root through `store.CreateNew()`, taken by `InsertRow` under the
exclusive hold of the full tail - and of the root, where CT7 holds it -
as `heap::ChainInsert` grows a declaration relation (`sys.assertions`,
`sys.transitions`, `sys.cabin_exprs`). What admits exactly one caller per id is the allocator under the
structure latch and the map latch: the free list first, then the cursor,
which starts at `kFirstUserPageId` and only rises, so a grown catalog page
is never below 128, and a freed user id taken here is a new creation
(invariant 2). The new page comes back pinned and exclusive and stays held
until its row's record stamps it (AY-S7), is stamped with the catalog
relation's oid (`page.md` §2a) and is logged with a `PAGE_INIT` carrying
that owner, so redo rebuilds it like any page; it is counted in the free
map and is an ordinary evictable frame (`eviction.md` EV3). `CreateNew`
may grow the file under those holds, as a user leaf's growth already
does; nothing in the allocator waits on a page latch. Ids 17..127 are
unused (`crosscore.md` CC9).

**A walk of a catalog chain is bounded by the walk budget**
(`storage::CheckPageWalkBudget`, `kMaxPageWalkLength` = 2^20 steps), the
bound every next-page loop owes, and not by a count of catalog pages: what
it stops is a cyclic link, and a long chain is not a fault. `ForFirstRow`
and `InsertRow` refuse a spent budget `Corruption` naming the chain's root.

A row no empty page can hold leaves its new page allocated, unlinked and
unreachable rather than freed - a free needs BF-R4's replay gate - a
stated leak of one page (`drop-table.md` DT1, `known-gaps.md`), the trade
`heap::ChainInsert` makes.

Placing a page at a *chosen* id stays every core's too: bootstrap's roots
and redo's heal call `CreateAt`, and `ClaimNamedIdLocked` under the same
two latches answers every loser `AlreadyExists` - a property of the claim
and not of the asking core (`page.md` §5). Pinned by
`free_map_race_test.cpp`'s `OnlyOneCallerEverPlacesAPageAtAChosenId`
(eight cores, one user-range id) and `device_page_store_test.cpp`'s
`APeerPlacesAPageAtAChosenIdInTheSystemRange` (the system range,
single-threaded).

## CT6 — The four words the instance keeps

`Expeditor` owns four atomics in one struct, `catalog::InstanceWords`
(`include/kds/catalog/instance_words.hpp`), and hands it to every core's
catalog by one setter, `Catalog::SetInstanceWords`: core 0's in
`Expeditor::Open`, a peer's through the one `CoreRuntime::Config` field
`instance_words`. Each word keeps its own rule in the table below - its
writers and its memory orders, stated at its field - and the struct is a
carrier only: no word orders another. What they share is the shape
`CoreRuntime::Config` already carries for the lock table and the instance
read view - `Expeditor` builds it, every core borrows a pointer, and a null
pointer leaves a bare catalog on its own local copy of each word (its own
oid counter, mark count and opted count, and no schema word, so it
revalidates nothing), which is what a fixture and the sim hold. A core's
transaction manager borrows the schema word alone, for a catalog-writing
decide (AT-S5e).

**One struct since CF-S1c, by threshold, not principle** (AT-0 item 11:
"three at three words, one at four"). At three words AT-S5b kept three
pointers, since a struct would have saved two setters and two `Config`
fields but re-opened AT-S2's schema-word wiring; the opted count made the
fourth and tipped it (the CF work order's CF-O-Q15, settled (a)).

| word | what it is | rule |
|---|---|---|
| **schema version** (`schema_word`) | the memo's generation counter | CT1, CT2 (AT-S2a) |
| **object-oid sequence** (`oid_sequence`) | the next oid `CREATE TABLE` and `CREATE NAMESPACE` issue | `catalog.hpp`'s contract at `GenerateUserOid`: every oid lands in `sys.objects` or `sys.columns`, which is what makes the seed recoverable (AT-S5b) |
| **delete-mark count** (`mark_count`) | the purge's gate | §5d of `ddl-transactional.md` (AT-S5b) |
| **opted count** (`opted_count`) | how many current-version `sys.patterns` rows carry `kPatternWaystone`; the `SELECT` path's gate, so an instance with nothing opted pays one load | moved only after a successful write under page 9's hold, on a 0->1 or 1->0 change of the bit on a current-version row, the decrement saturating at 0; set from the chain at every boot (the mount's renewal, else `RecountOptedPatterns`); acquire reads, acq_rel moves, a release store at the boot; it publishes nothing beside itself, the opted map being the schema word's to order. A stale count costs performance only (invariant 8) (CF-S1c; `waystone-concpets.md`) |

**The oid sequence is seeded once, by whichever catalog asks first**, with a
CAS from zero to `HighestIssuedUserOid() + 1`; zero is the unseeded value
and no oid can be zero, so one word carries both states. Two cores seeding
at once agree - one CAS wins and the loser's read is the winner's. A
per-catalog counter would issue one oid twice: each catalog seeds lazily,
so a core's second create issues from a counter seeded before another
core's rows existed.

**The mark count is a delta at the sweep, not an assignment.** The purge
resettles the count to what it saw unsettled; with more than one core
marking, a plain store would drop a mark another core added while the
sweep walked. It adds `remaining - before` instead, which leaves a
concurrent mark counted, can overshoot by a mark the sweep both saw and was
told about - the direction the gate's own rule already allows, costing one
sweep that finds less than it expected - and cannot undershoot, which is
the direction that would strand a mark until the next mount.

## CT7 — A name is taken under its relation's root page

**Every write that gives a catalog row a name makes its binding check and
its write under one hold of that relation's root page, taken exclusive**
(AT-S17; the AT-close order's Q2, marked 2026-09-26: the held page, no
name lock unit). A DDL runs where its session is, so a check that was a
read and a write that was a second latch hold would let two cores both find
a name free and both write it — two rows claiming one name, and resolution
answering whichever it met first. Every *new* row of a catalog relation enters its chain at the root,
so a writer holding the root is the only one adding a row; an in-place
rewrite (a rename, a retype, a rollback's compensation by page and slot)
does not pass through it, and what keeps those from admitting a name is
what the check asks, below, not the latch. `RecordAccess` (page 11) and
the pattern registration of the time (page 9) were the shape, for their
admissions, at AT-S7; since CF-S1b page 9's hold is `SetPatternWaystone`'s,
the one inserter of a `sys.patterns` row (`waystone-concpets.md` §4a).

| write | relation (root page) | held across |
|---|---|---|
| `CreateTable` | `sys.objects` (6) | the binding check and the `sys.objects` insert. The same check is asked unheld at the top, so a taken name costs no oid and no page; a create that loses under the hold has burned an oid and its pages, as a failure anywhere after them does. A relation's name is asked in the namespace it is created in (`namespace.md` NS6) |
| `CreateNamespace` | `sys.objects` (6) | the check and the insert; the oid is issued before the hold |
| `RenameTable` | `sys.objects` (6) | the check and the in-place rewrite; the new name is asked in the relation's own namespace |
| `RenameColumn` | `sys.columns` (5) | the sibling check and the rewrite — the relation `X` both renames take (`alter.md` AL4a) already orders two of one relation; the hold stays as defense in depth, and is the whole of it for a dispatcher with no lock table |
| `CreateIndex` | `sys.indexes` (8) | the check and the insert; `CheckIndexDef` asks it unheld, earlier, so a taken name is refused before a build |
| `InsertAssertion` | `sys.assertions` (14) | the check and the insert; `AssertionBuild::ForCreate` asks unheld before its build and `Finish` again before its adoption, for the same reason |
| `CreateCabin` | `sys.cabins` (12) | not a name but the same shape — one cabin per column — and reached by two operators, or an operator and the controller; the id is issued before the hold |

**A relation's name has one copy, its `sys.objects` row**, and every view
reads it there. `sys.tables` carries none: the 64 bytes at
`SysTableRow::kRetiredNameOffset` are written 0 and never read, so the row
size and the superblock stand, and whatever an older volume holds there is
ignored.

**What is checked, in one walk per relation** (`Catalog::CheckNameFree`,
`Catalog::CheckIndexNameFree`), so no caller can ask half of it:

- **A live row of the name, unfiltered** — `ddl-transactional.md`'s
  duplicate check: another transaction's uncommitted row counts, whichever
  core wrote it, because a row is on the page from the moment it is
  written. Read off the page, never the name cache. `AlreadyExists`;
  `CREATE TABLE` answers it `EXISTS oid=`, and so does a create that lost
  the name under the hold having written nothing.
- **A drop that has not committed.** A table's or a namespace's drop
  retypes its `sys.objects` row in place and an index's delete-marks its
  `sys.indexes` row; either frees the name at once, and either's rollback
  restores it beside whatever took the name meanwhile. Asked of the row's
  stamp under an **instance** check view (`MintCheckView`, AN-S2), so a
  drop open on any core is seen. Another transaction's drop is
  `TxnConflict`: the name is free once that transaction resolves, which is
  what the retryable code means. **The asker's own drop** is told apart by
  its stamp, and answered by what the asker is:
  - a create of a table or a namespace takes the name
    (`DROP TABLE t; CREATE TABLE t` in one transaction, which one rollback
    undoes whole);
  - a create of an index takes the name since BY-R8
    (`DROP INDEX ix; CREATE INDEX ix` in one transaction, which one
    rollback undoes whole) - the rule `REINDEX`'s swap rests on; it was
    `AlreadyExists` from AT-S17 until then;
  - a rename is `Unsupported` — it lands for good, and the drop's rollback
    would restore the name beside it; commit the drop first.

  Background: before AT-S17 each case was asked core-locally or not at all,
  and read another core's open drop as done. Asked in the catalog, the SQL
  `CREATE TABLE`'s refusal comes after its column types and foreign keys
  are resolved.

**A column name is a relation's own, and unique ignoring case**
(`catalog::ColumnNamesCollide`, the one rule): `CreateTable` refuses a
schema two of whose columns' names are equal ignoring ASCII case
(`CheckColumnNamesUnique`, `InvalidArgument`), and `AddColumn` and
`RenameColumn` refuse a name equal ignoring case to a live column's
(`AlreadyExists`, naming that column as declared). A dropped column's name
is free in any case (BW-R6) - once its drop is decided, for
`RenameColumn`, whose rewrite is transactionless. `RenameColumn`
resolves the name it is given as every statement does - ignoring case, the
one live column it matches (`parser-v2.md`'s resolution rules;
`instructions/v3.2.0/raft-marks-2026-10-15.md` §1) - and writes that column
by its position. `DropColumn` resolves nothing: it takes the position the
dispatcher resolved the name to and asked its RESTRICT of, and refuses
`TxnConflict` a position no longer carrying the name ignoring case. A
RENAME COLUMN takes the relation `X` since AL4a (`alter.md`), as `DROP
COLUMN` does before its ask, so none lands between the ask and the write;
the position is kept as defense in depth - a second resolution by name
under the hold, with no lock table, could reach a column renamed onto the
name in between - one the RESTRICT was never asked of `[quiet-wrong]`. `RenameColumn`'s collision scan skips the column
being renamed, so `a` may become `A` (`RENAME COLUMN A TO Id` on `a` is a
rename too), while a rename onto the name the column already has, byte for
byte, is `AlreadyExists`. A relation created before the rule may still hold
two such names, or two equal ones (CREATE TABLE once admitted `(a, a)`); a
statement naming them is refused as ambiguous, and `RenameColumn` refuses
a name two live columns match ignoring case (`InvalidArgument`), as its own
defense - so no statement can name either column of such a pair. A column name
is at most 63 bytes (`kCatalogNameMax - 1`): CREATE TABLE and ADD COLUMN
refuse a longer one at its byte, as `RenameColumn` does, rather than store
it truncated.

**Assertion names have no pending drop to ask about**: `DROP ASSERTION`
retires its row outside any transaction (`ddl-transactional.md` §5).

**What nests under a hold** (`rules.md` §3): the relation's own chain
growth, the allocator for it, the WAL, a transactional write's undo page
(a sink that asks no catalog page), and for `sys.assertions` its own
var-heap chain — never another catalog relation's page; `InsertDeclaration`'s
`OpenDeclarations` under page 14 is a cache hit, filled before the hold
(`InsertDeclaration`, `declaration_relation.cpp`). That is why the oids and ids above are
issued before their holds: `GenerateUserOid`'s first use seeds from
`sys.columns`, and `AllocateRowId` writes `sys.tables`. `SetPatternWaystone`
(`WAYSTONE ON`, CF-S1b), outside this table, issues under page 9 and so takes page 7 under it;
nothing takes page 9 under page 7 (`rules.md` §3's catalog row).

**A `sys.tables` chain page is inner to every user relation page**
(BB-R4, declared in `page.md` §6), so no hold here may ask for a user
relation page.

**What it does not cover.** The hold holds a name only until its latch
drops; what keeps a later check from admitting a name an open transaction
has taken or given up is the two questions above, not the latch.

**A relation's namespace is taken under the same hold** (AT-S17b, the
AT-close order's §7.3). A relation's `sys.objects` row carries its
`namespace_oid`, so membership is written where the name is. `CreateTable`
asks, under page 6's hold, that the namespace it was handed is still there
(`CheckNamespaceLive`); `DropNamespace` holds page 6 across its RESTRICT
check and its retype, and the check reads **`sys.objects`**
(`CheckNamespaceEmpty`) rather than `sys.tables`, which a create writes after
page 6's hold drops. The create is refused, or the drop sees its row - on any
core. Both use `CheckNameFree`'s walk and instance check view, so an undecided
drop counts across cores: a namespace another transaction is dropping -
or is still creating, whose rollback would leave the relation in nothing
- is `TxnConflict` to a create; a relation another transaction is
dropping is still in its namespace, `TxnConflict` to the RESTRICT (the
asker's own drop `InvalidArgument`). `CreateTable` refuses a namespace oid
no row names.
