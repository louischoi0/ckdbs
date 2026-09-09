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
the compiler between the name and the schema (AT-S1; `txn.md` §5,
`read_borrow.hpp`). DDL takes the relation `X`. While a statement's `IS`
is granted, no DDL can take the `X` until the statement ends, so a
statement that has resolved a schema is never overtaken by a change to
it. **That is what makes a stale parse unexecutable, and it is the only
thing that does.** The defence is one-directional — a statement arriving
while the DDL already holds `X` is refused its `IS` and reads on, and
what makes *that* statement's answer right is `drop-table.md` DT1, catalog
MVCC and the absence of data-moving DDL, never the word.

The **schema version word** saves a re-parse and nothing else. One
`std::atomic<uint64_t>` for the instance, owned by `Expeditor`,
memory-resident and never persisted — every cache is empty at mount, so no
value survives one. Every core's `Catalog` holds a pointer to it
(`Catalog::SetSchemaWord`). It is not `Catalog::catalog_version()`, which
is per-instance and which a peer's drop never advanced — that is why
`range_directory.hpp` forbids validating against it — and the two are not
merged: the word is a generation counter **every** invalidation bumps.

## CT2 — Bumped at the write, asked at the boundary

**Bumped at the catalog write**, by `Catalog::BumpVersion()`, the single
DDL choke point, which is before the DDL's relation `X` is released at its
decide (AR0-5 D21 as marked). Both endings of a catalog-writing
transaction bump again through `InvalidateAfterCompensation()` — a
rollback because the rows were compensated behind the cache's back, a
commit because a delete-mark starts counting then (`ddl-transactional.md`
DT9). The one write that keeps its own cached entry across a bump is the
key-order flip, because the `INSERT` doing it holds the relation's
`TableAccess*`; it bumps like any other, and other cores re-read the flag.

**Asked at a task boundary, and never inside one.** `Catalog::Revalidate()`
is one acquire load; on a mismatch the cache is dropped and the value
adopted. It runs where the broadcast's handler used to — between tasks:
`CommandDispatcher::DispatchAndStage`'s head, the remote-step, FK-probe
and build request handlers, the `system` ticks that read the catalog.
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

A writer's own cache is not dropped by its own bump: `BumpWord` records
the new value as what the cache was built at — **only if the cache was
current**. A cache that is behind the word and bumps it does not adopt,
so a change it never revalidated against still drops at its next
boundary; otherwise a peer's own write, once one exists (AT-S3), would
swallow another core's DDL and serve a stale schema for good.

## CT3 — What a task that asks nothing serves

A task that reads the catalog without a boundary serves a memo at most
one DDL stale — what a peer served while a broadcast was in flight.
Stale, never wrong, by CT1's three facts. Where staleness has a cost the
task asks: the peer's refill tick, whose stale answer is a range that
does not open.

## CT4 — Why a drop of the memo is enough

One frame table serves every core (AM-S2 step 3, `page.md` §3): the
re-read after a drop finds the bytes the writer wrote, under the page
latch. On a per-core pool it would re-read the same stale frame and
conclude the same nothing, which is why the broadcast's handler evicted
frames as well as dropping facts. The flush that preceded the broadcast
carried the pages to the *device* for such a re-read; with one pool it
moved bytes nobody re-read from there, and it is gone with the broadcast.
The catalog pages are still unlogged, so a fixture that reads the device
still flushes; the engine does not.

## CT5 — What stays special about the catalog's pages, and what does not

Two things, neither an authority (AR0-5 §2, AT-R11): their frames are
pinned, and page 0's address with the fixed catalog page numbers is
bootstrap layout. Until AT-S3 the pages have one writer, core 0, and
`Revalidate()` is a no-op there — the single writer's `cache_built_at_`
always tracks the word — so every effective drop the word makes today is
a peer's. AT-S3 makes a `sys.tables` row borrowable at the tuple unit and
AT-S5 lets any core write the pages; nothing in this file changes for
either, which is the point of writing it before them.
