# Catalog — the schema version word, and what it is not

Opened 2026-09-09 at AT-S2c (`instructions/v3.0.0/workorder-at-m3-uniformity.md`),
on AR0-5 §2.1's instruction that the order below be *"written in that
order in `catalog.md` so nobody later removes the lock because the word
'covers it'"*. This file owns one thing: how every core's view of the
catalog stays current once no core is told anything. The catalog's rows,
pages and DDL semantics stay where they are — `heap-and-tuple.md` §4,
`ddl-transactional.md`, `alter.md`, `drop-table.md`.

## CT1 — The lock is the argument; the word is the fast path

A statement holds a relation `IS` on every relation it binds, taken by
the compiler between the name and the schema (AT-S1). **`DROP TABLE` takes
the relation `X`** (`drop-table.md` DT7), and while a statement's `IS` is
granted the drop waits for the statement to end. That is the lock's whole
reach, and `txn.md` §5 owns it: every other DDL is catalog-only and takes
no relation lock — a rename, an index or assertion build commits under a
bound statement's `IS` — and the foreign-key check family declares
nothing (`txn.md` §5, AT-S1's stated omission). Under those, as under a
refused `IS`, what makes a statement's answer right is `drop-table.md`
DT1, catalog MVCC and the absence of data-moving DDL, never the word.
**The order this file exists to state**: the lock is the argument where it
reaches, the word is the fast path everywhere, and nobody removes the lock
because the word "covers it".

The **schema version word** saves a re-parse and nothing else. One
`std::atomic<uint64_t>` for the instance, owned by `Expeditor`,
memory-resident and never persisted — every cache is empty at mount, so no
value survives one. Every core's `Catalog` holds a pointer to it
(`Catalog::SetSchemaWord`). **It is not `Catalog::catalog_version()`**,
which is one core's counter (`catalog.hpp`) that a peer's drop never
advanced — the reason `range_directory.hpp` forbids validating a range set
against it, and the one home of that rule — and the two are not merged:
the word is bumped by every invalidation that can stale another core's
memo. (Two that cannot do not bump it: the mount's post-redo `DropCache()`
and its delete-mark purge.)

## CT2 — Bumped at the write, asked at the boundary

**Bumped at the catalog write**, by `Catalog::BumpVersion()`, the single
DDL choke point, which is before the DDL's relation `X` is released at its
decide (AR0-5 D21 as marked). Both endings of a catalog-writing
transaction bump again through `InvalidateAfterCompensation()` — a
rollback because the rows were compensated behind the cache's back, a
commit because a delete-mark starts counting then (`ddl-transactional.md`
§5b). The one write that keeps its own cached entry across a bump is the
key-order flip, because the `INSERT` doing it holds the relation's
`TableAccess*`; it bumps like any other, and other cores re-read the flag.

**Asked at a task boundary, and never inside one.** `Catalog::Revalidate()`
is one acquire load; on a mismatch the cache is dropped and the value
adopted. It runs where the broadcast's handler used to — between tasks, at
nine sites: `CommandDispatcher::DispatchAndStage`'s head; the remote-step
open (`OnStepOpen` — not the batch handler, whose parked producer
re-`Bind`s instead); the two foreign-key probe handlers; the two build
request handlers; the peer's refill tick and that refill's completion,
which reads the range core 0 just opened; and the Cabin optimizer's tick.
**It is never called from a cached read**, because a drop frees every
`const TableAccess*` and `Schema&` a running statement holds
(`catalog_cache.hpp`'s entries are reference-stable *until the next
drop*), and a drop inside a statement is a read through freed memory —
a peer's join binding its second relation, an FK loop over a borrowed
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
as its last boundary — what a peer served while a broadcast was in flight,
which bounded nothing either. Stale, never wrong, by CT1's three facts.
Where staleness has a cost the task asks: the peer's refill tick and its
completion, whose stale answer is a range that does not open, and the
Cabin optimizer's tick.

## CT4 — Why a drop of the memo is enough

One frame table serves every core (AM-S2 step 3, `page.md` §6): the
re-read after a drop finds the bytes the writer wrote, under the page
latch. On a per-core pool it would re-read the same stale frame and
conclude the same nothing, which is why the broadcast's handler evicted
frames as well as dropping facts. The flush that preceded the broadcast
carried the pages to the *device* for such a re-read; with one pool it
moved bytes nobody re-read from there, and it is gone with the broadcast.
Catalog writes are WAL-logged like every other (`ddl-transactional.md`
§7), so no page-level force is owed at a DDL's commit — redo carries the
page; a fixture that opens a second store over the device still flushes,
the engine does not.

## CT5 — What stays special about the catalog's pages, and what does not

Two things, neither an authority (AR0-5 §2, AT-R11): their frames are
pinned, and page 0's address with the fixed catalog page numbers is
bootstrap layout. Until AT-S5 the pages had one writer, core 0, and
`Revalidate()` was a no-op there — the single writer's `cache_built_at_`
always tracked the word. Every core writes them since AT-S5, so every
core's cache can now be behind the word and `BumpWord`'s adopt-only-if-
current rule (CT2) is live on every core rather than latent; nothing else
in this file changed for it, which was the point of writing it first.

**A catalog row is not a lock unit** (AT-S3, E13 answered no). A named
key's admission writes the relation's `sys.tables` row - the mark, or the
key-order flip - outside the caller's transaction (`heap-and-tuple.md`
§4.1: both writes outlive a rollback), and neither write has a contention
problem a lock would fix. The mark is a monotone number nobody caches:
its read-modify-write is atomic because **no task parks under a page
span** (`page.md` §6's discipline; `AdmitExplicitRowId`'s `before_mark`
hook runs inside one and says so) — the page latch serialises cores, and
it is re-entrant for the owning core, so within one core it is the
no-park rule and nothing else. The flip is the one catalog write whose
*staleness* is a wrong answer — a stale `kAscending` elides an
`ORDER BY <pk>` and answers out of order — and staleness is what the word
fixed at AT-S2: the flip bumps it. A transaction-length `X` would
serialise every named-key `INSERT` into a relation for the length of each
transaction and protect nothing. A peer refused a named key until AT-S5
(`catalog_read_only_`); it admits one now, the page write being every
core's. `rules.md` §3 declares the pages, not the rows.

**Placing a *new* catalog page is every core's too, since AT-S5b.** The
pages CT5 opens with are the fixed ones; a catalog relation's chain grows
past them into the reserved overflow range (`kCatalogOverflowFirst` ..
`kCatalogOverflowLimit`, `include/kds/catalog/well_known.hpp`; the rule that
a catalog page may be managed past the fixed ones is `crosscore.md` CC12
CR3), and `AllocateCatalogPage`
walks that range id by id, taking the first that is free. `CreateAtUnpinned`
refused a chosen id below the resident limit to every core but 0 until
AT-S5b - the last core-0 write predicate the store had, written as
unreachable and made reachable by AT-S5, and in the wrong direction: the
probe loop treats only `AlreadyExists` as "try the next id", so on a peer
the *first* probe failed and every catalog chain that had to grow failed
permanently. What admits exactly one caller per id is the claim itself -
`ClaimNamedIdLocked` under the structure latch and the map latch, which
answers every loser `AlreadyExists` - and that is a property of the claim
and not of the asking core (`page.md` §5). **Two cells between them, and
neither alone**: `free_map_race_test.cpp`'s
`OnlyOneCallerEverPlacesAPageAtAChosenId` races eight cores for one id and
never touched the retired arm, its ids being in the user range;
`device_page_store_test.cpp`'s
`APeerPlacesAPageAtAChosenIdInTheSystemRange` reads the system half and is
single-threaded.

## CT6 — The three words the instance keeps

`Expeditor` owns three atomics and hands each to every core's catalog. They
arrived one stage apart, and each keeps its own rule in the table below.
What they share is the shape `CoreRuntime::Config` already carries for the
lock table and the instance read view - the owner builds it, every core
borrows a pointer, and a null pointer leaves a bare catalog on its own
local copy, which is what a fixture and the sim hold.

**Three pointers rather than one struct, and the reason is a threshold and
not a principle.** The three are identical in lifetime, plumbing, default
and wiring sites, and they are mirrored in three fixtures, so each new word
costs about a dozen edits - a struct would collapse those to one. What it
would also do is re-open AT-S2's shipped schema-word wiring, which is a
landed stage's declared seam (CT1, CT2), for a saving of two setters and
two `Config` fields. At three words that does not pay; at four it does, and
AT-0 item 11 is where the fourth decides it. The argument first written
here - *"three facts with three owning rules, and a stage that moves one
should not have to move the other two"* - was refuted by AT-S5b's own
review and is recorded rather than kept: a struct creates no shared *rule*,
each field keeping its comment and its owning doc, and removing a field
from a struct is one deletion rather than three.

| word | what it is | rule |
|---|---|---|
| **schema version** | the memo's generation counter | CT1, CT2 (AT-S2a) |
| **object-oid sequence** | the next oid `CREATE TABLE` and `CREATE NAMESPACE` issue | `catalog.hpp`'s contract at `GenerateUserOid`: every oid lands in `sys.objects` or `sys.columns`, which is what makes the seed recoverable (AT-S5b) |
| **delete-mark count** | the purge's gate | §5d of `ddl-transactional.md` (AT-S5b) |

**The oid sequence is seeded once, by whichever catalog asks first**, with a
CAS from zero to `HighestIssuedUserOid() + 1`; zero is the unseeded value
and no oid can be zero, so one word carries both states. Two cores seeding
at once agree - one CAS wins and the loser's read is the winner's. A
per-catalog counter was sound while core 0 alone created objects and issued
one oid twice the moment a peer ran `CREATE TABLE`: each catalog seeds
*lazily*, so the first create on each core reads the other's rows and the
**second** issues from a counter seeded before they existed.

**The mark count is a delta at the sweep, not an assignment.** The purge
resettles the count to what it saw unsettled; with more than one core
marking, a plain store would drop a mark another core added while the
sweep walked. It adds `remaining - before` instead, which leaves a
concurrent mark counted, can overshoot by a mark the sweep both saw and was
told about - the direction the gate's own rule already allows, costing one
sweep that finds less than it expected - and cannot undershoot, which is
the direction that would strand a mark until the next mount.
