# The FK intent and pending-delete tables are per core, and the deleter is no longer the core that registered

**Found** 2026-09-09 by the `critics-developer` pass on AT-S5
(`091be8c` on `m3-at`), as its D5. **Not fixed.** Verified at `091be8c`;
the fix's shape is decided (operator, 2026-09-09) and its stage is
**AT-S5f**.

**Cost: a quiet wrong answer.** The two tables are what stop a parent from
being deleted while another transaction is writing a child against it, and
each is read on a core that may not be the one it was written on — so a
`DELETE` meets an empty table and proceeds.

## The wrong behaviour

Both are by-value members of `CoreRuntime` — `FkIntentTable fk_intents_`
(`include/kds/server/core_runtime.hpp:745`) and `FkPendingDeleteTable
fk_pending_deletes_` (`:749`) — with a second, unrelated pair on `Expeditor`
for core 0 (`include/kds/server/expeditor.hpp:934`, `:939`). Each is wired
to its own core's dispatcher and probe server only
(`src/server/core_runtime.cpp:811, 850, 854`;
`src/server/expeditor.cpp:2003-2004, 2036-2037`). The headers say so:
*"One table per core; every map below is that core's own"*
(`include/kds/server/fk_intent.hpp:66-67`), *"Concurrency: core-local, no
synchronization"* (`:246`).

The two sides are written on **different** cores:

- the reference intent is granted on the **parent's owner**, by the probe
  server (`src/server/fk_probe_service.cpp:190`), because the forward check
  still defers a foreign parent by `owner_core`
  (`src/server/command_dispatcher.cpp:4733`);
- the pending delete is registered on the **deleting** core, before the
  fan-out (`src/server/command_dispatcher.cpp:5263-5268`).

And both are read on the core running the statement:
`pending_deletes_.Pending(...)` at `fk_probe_service.cpp:144-148`,
`fk_intents_->HeldByAnotherThan(...)` at `command_dispatcher.cpp:5279` and
`:5345-5353`. The comment at `:5339-5344` states the premise that has
expired: *"an intent lands here only from a core whose extraction pass found
this parent foreign — and a statement on this core never defers a parent
this core owns."* Since AT-S5 a `DELETE` of that parent runs wherever the
session is, so it reads a table the intent never reached.

## Reproduction

Two cores, a parent relation and a child referencing it. On core 1, begin a
transaction and insert a child row whose parent lives on core 0 — the probe
leaves an intent on core 0. On **core 1**, delete that parent: the deleting
core's own intent table is empty, so nothing answers busy, and the parent
goes while an uncommitted child references it.

## The fix

`ResolveForeignKeyParents` stops deferring by `owner_core`: every parent is
read here through the one pool that has served every core since AM-S2 step
3, and an in-flight parent is the same-core busy the engine already turns
into AO-S3's wait — now reaching across cores because the lock table is the
instance's. Both tables and the probe protocol they exist for retire with
it, and `fk_probe_service`'s request path becomes the dead protocol AT-S10
removes. This is the shape AT-5's own S5 row listed as a cell — *"the FK
forward check meets a parent being written on another core and waits —
AO-S5(b)'s cell, re-pointed at a local wait"* — and did not build.

The alternative, unifying the two tables on `Expeditor` and keeping the
probes, was weighed and declined: it carries a protocol AT-3 C already
assigns to AT-S5 for retirement.

## Owner

`instructions/v3.0.0/workorder-at-m3-uniformity.md`, AT-S5f.
`docs/spec/foreign-keys.md` §2a, §2b and §3a are the sections that state the
retiring protocol.
