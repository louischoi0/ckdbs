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
