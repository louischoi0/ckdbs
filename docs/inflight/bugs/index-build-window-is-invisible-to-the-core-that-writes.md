# A `CREATE INDEX` opens a window no other core can see, and a local one opens none

**Found** 2026-09-09 by the `critics-developer` pass on AT-S5
(`091be8c` on `m3-at`), as its D6. **Not fixed.** Verified at `091be8c`;
the fix's shape is decided (operator, 2026-09-09) and its stage is
**AT-S5e**.

**Cost: a quiet wrong answer.** A row inserted while an index is being
built is absent from the finished index, and the index reports itself
complete. A descent then misses a row that exists — which is the one thing
`heap-and-tuple.md` §5 says a btree descent may never do — and the relation
and its index disagree until the index is dropped and rebuilt.

## The wrong behaviour, in two halves

The window is `PendingIndexBuilds`, a **per-`CoreRuntime`** member
(`include/kds/server/core_runtime.hpp:661`), wired to that core's dispatcher
(`src/server/core_runtime.cpp:492`) and to its `IndexBuildServer` (`:669`).
It exists so that a write meeting a build in flight is parked and re-run
rather than admitted into a relation whose index is half-built:
`CheckWriteAffinity` at `src/server/command_dispatcher.cpp:6824-6838`
records `index_window_wait_` and returns `IndexBuildPending`, awaited by
`AwaitIndexWindow` (`:513-580`).

1. **A local `CREATE INDEX` opens no window at all.** The only `Open` in the
   tree is `src/server/index_build_service.cpp:143`, on the shipped-request
   path. The local build — `exec::CreateIndex` at
   `src/server/command_dispatcher.cpp:3377-3379`, which runs `BuildIndexTree`
   and `Backfill` — opens nothing and closes nothing. Before AT-S5 that was
   sound: a build ran locally only on the relation's owner, and every write
   to that relation was routed to the same core, where the reactor
   serialises them. Since AT-S5 a write runs where the session is.

2. **Core 0 has no window to read.** `Expeditor` never calls
   `SetPendingIndexBuilds` — it installs only the *client*
   (`src/server/expeditor.cpp:1886-1888`) — so core 0's dispatcher pointer is
   `nullptr` and its `CheckWriteAffinity` gate is dead. A shipped build on a
   peer is therefore invisible to every write running on core 0, which
   since AT-S5 is any write whose session was accepted there.

## Reproduction

Two cores, one populated relation. Start `CREATE INDEX` on core 0 (or ship
one to core 1); insert a row on the other core while the backfill runs; the
insert is admitted, and the finished index does not contain it.

## The fix

`CREATE INDEX` and `DROP INDEX` take the relation `X`, the way `DROP TABLE`
has since AO-S6e-b: the build waits for open writers and positioned readers
under the lock family's fault net, and the schema word bumps at the catalog
write before `X` is released (D21), so a core that arrives after the build
re-parses. The whole window machinery — `PendingIndexBuilds`,
`AwaitIndexWindow`, `IndexBuildPending`, `kIndexWindowWaitNs` — retires with
it, and so does `DROP INDEX`-in-a-transaction's `NotImplemented`
(`command_dispatcher.cpp:3310`), whose reason is DT9's core-local
"is the deleter in flight" predicate: the `X` waits for the deleter instead
of refusing on its account.

**AO-S6e-a declined this same relation `X`** and its reason is worth
recording, because it has since expired: the owner's window close, its
catalog-cache drop and its next admitted write were one ordered event that a
lock released on core 0 could not reproduce. AT-S2a's schema word supplies
exactly that ordering — bump at the write, ask at the task boundary
(`docs/spec/catalog.md` CT1, CT2) — so the objection no longer holds.

## Owner

`instructions/v3.0.0/workorder-at-m3-uniformity.md`, AT-S5e.
`docs/spec/index.md`'s build section and `docs/spec/crosscore.md` CC7's
owner-builds exception are the rules that go with it.
