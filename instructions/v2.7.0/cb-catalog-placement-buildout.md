# Work order CB — catalog placement build-out

Drafted 2026-08-31 against `main` at `6b5b368` (`v2.2.1-142-g6b5b368`).
Governed by `instructions/v2.7.0/r1-catalog-placement-ratification.md`
(CR1-CR4) and its addendum CR5-CR8 below. Supersedes
`r1-every-core-equivalent.md`, withdrawn by CR4 and **marked closed and
rejected** on 2026-08-31 — that file is now a tombstone stating why.

## Addendum to the ratification — CR5-CR8

Ratified by the operator 2026-08-31.

**CR5 — DDL dispatches to core 0.** A peer detects a DDL verb at
dispatch and routes it to core 0 rather than refusing it. The peer
waits for the outcome.

**CR6 — statistics pages are unlogged, as a named exception.** A
catalog relation whose only content is a statistic is written without
WAL records. This is an **exception to RV3**, which established
(`ddl-transactional.md` §7) that catalog writes are WAL-logged as
ordinary record types and replayed. The exception is to be written into
the rules, not left as a local decision — see CB9.

**CR7 — a peer batches statistics locally and flushes periodically.**
Not one message per statement.

**CR8 — a full ring drops the batch.** Invariant 8 already prices this
trail as performance and never a result, so a drop is a permitted
outcome and not an error to report.

## Standing instruction

Where a decision arises below that CR1-CR8 do not settle, CLA's proposal
is accepted by the operator's standing instruction of 2026-08-31. Each
such point is marked **[PROPOSAL]** and carries CLA's reasoning, so that
a later reader can see it was proposed rather than measured. **Constants
are not covered by this instruction** — no batch size, interval or cap
is decided here; CB7 measures them.

## Scope

**In.** CB0-CB3: `sys.pattern_defs`' var-heap, CR1's first exercise.
CB4-CB6: DDL dispatch to core 0 under CR5. CB7-CB8: batched, unlogged
peer statistics under CR6-CR8. CB9: the rule text CR6 requires.

**Out.** The mover. `sys.ranges`' write path. Reclamation of anything.
Migrating any other catalog relation out of the reserved range — CR3
permits it; this order performs it for nothing beyond CB0-CB3.

**Note what CR7 removes from the earlier draft.** `sys.access_stats`
stays at `kCatalogPageAccessStats = 11`, one relation, core-0-written.
So the per-core-relation questions the withdrawn draft could not start
without — oid allocation, migrating page 11's rows, what a core-count
change does to a departed core's relation — do not arise. That last one
matters most: CR7 adds no dependant to `wal.md` §53's `[OPEN]` "how",
which the earlier scoping did.

---

## CB0-CB3 — CR1 on `sys.pattern_defs`

**CB0. Establish what a peer actually hits.** `sys.assertions` reaches
its var-heap through `exec::AssertionSpillPages`, granting the individual
pages a row names. `sys.pattern_defs` has the same shape and no peer
reader, so the failure is latent. Write the failing case first — a peer
resolving a pattern whose definition spilled — and record the exact
status and message. Do not assume `may not fault page N`; read it.

**CB1. Mechanism. [PROPOSAL]** Two candidates:

- **Page-at-a-time grant**, as `sys.assertions` does. Deliberately not
  an extent grant, because an extent covers pages that core owns and
  costs it PW1c-7's stamp-claimed write rights.
- **A reserved low sub-range for catalog var-heap**, which CR1's text
  calls the general answer and which is a **format change**.

**CLA proposes the page-at-a-time grant**, on three grounds: it is
already built and proven for `sys.assertions`; a pattern definition is
read at DDL frequency rather than per statement, so the grant round trip
does not sit on a hot path; and a format change made before CB0's
measurement exists would be a constant decided without measurement. The
format change stays available and CR1 stays the rule that would justify
it. **If CB0 shows the read is per-statement rather than per-DDL, stop
and report** — the proposal rests on that frequency and not on
convenience.

**CB2. Build it for `sys.pattern_defs` alone.**

**CB3. Amend `well_known.hpp`.** The comments at 277-282 and 289-293
describe var-heap placement as a consequence of `CreateNew()`. After CR1
it is a rule. Amend both and add CR1's note: a catalog relation that
gains a var-heap puts it outside the reserved range by decision.

---

## CB4-CB6 — CR5, DDL to core 0

**CB4. Turn the guard into a route.** The detection half exists.
`command_dispatcher.cpp:909` already keys on `catalog_read_only_` and
the tokens `CREATE`/`ALTER`/`DROP`, and its comment states the token is
deliberately the same one the routing below reads *"so the two cannot
disagree about what a verb is"*. CR5 changes what happens after the
match: `ShipStatement` to core 0 instead of `ErrorReply(PeerDdlRefused)`.

Keep `PeerDdlRefused` and its call site reachable — CB5 leaves cases
that still refuse, and the comment's warning that this is the only guard
under NDEBUG stands for those.

**CB5. Autocommit only in this phase. [PROPOSAL]** A DDL statement
inside an explicit transaction continues to be refused and to poison,
exactly as today.

CLA's reasoning: shipping inside a transaction enrols core 0 as a 2PC
participant (`ShipStatement`'s `EnrolParticipant`). RV3 made DDL
transactional and D2 gave autocommit DDL an implicit transaction, so
`CREATE`/`DROP TABLE` would likely behave — but `known-gaps.md` records
`ALTER TABLE`, cabin, pattern, assertion and FK as **still
non-transactional**, and `DROP TABLE` as atomic but not isolated.
Enrolling a participant that holds nothing to prepare is a worse failure
than a refusal the client can see. The transactional case is a separate
row once those relations are transactional, and it is not this order's.

**CB6. `CREATE` has no oid to ship. [PROPOSAL]** `ShipStatement`'s
`oid` parameter keys D4's dedup record on the owner. `DROP` and `ALTER`
name an existing relation; `CREATE` does not.

**CLA proposes shipping `kSysTablesTable` as the oid for `CREATE`** —
the relation core 0 will in fact write — rather than adding a DDL
request kind. Reasoning: the dedup record's purpose is recognising a
duplicate of *this* statement, and `ShipStatement` already mints a
session ship-id and a per-session sequence that carry that identity;
the oid is the owner-side routing key, and for DDL the owner is core 0
by CR5 regardless of oid. A new request kind is a wire addition for no
behaviour CR5 needs.

**Verify one thing before relying on it**: that the owner-side dedup
path keys on (ship_id, sequence) and treats oid as routing only. If it
keys on oid in a way that would collide two different `CREATE`s from
one session, the proposal fails and a request kind is the answer.

**Catalog invalidation is already built — do not rebuild it.**
`RingMessageKind::kCatalogInvalidate = 19` (core 0 → all) is broadcast
and handled (`expeditor.cpp:1132`, `core_runtime.cpp:418`). CR5 makes it
reachable far more often than before. Two things follow that CB6 must
check rather than assume: `catalog.cpp:1128` states plainly that this
broadcast *"is not all of CC10"*, so establish what it does not cover
before a peer's DDL depends on it; and `core_runtime.cpp:428` argues a
statement racing the invalidate is fine, an argument written when only
core 0 issued DDL. Re-read it under CR5 and say whether it still holds.

---

## CB7-CB8 — CR6-CR8, batched unlogged statistics

**CB7. Build the peer-side batch and the ring path.** A peer accumulates
`(core_id, target_oid, kind, column_mask, count, last_seen)` locally and
flushes to core 0 periodically. Core 0 applies the batch to
`sys.access_stats` at page 11. A full ring drops the batch (CR8),
silently as far as the client is concerned, and increments an
observability counter so a drop is visible in `SHOW META` rather than
invisible.

**Constants are measured, not proposed.** Batch buffer size and flush
interval are exactly the kind of number `known-gaps.md` exists to catch.
Sweep them; the operator takes the values. Two things to measure
against: `RecordAccess`'s present cost is +1-2% on a point lookup
(`heap-and-tuple.md` §7), which is the ceiling a batched path must beat;
and core 0 is already the system core for free map, extent leases,
trx-id leases, DDL and `sys.ranges`, so **report what the batch adds to
core 0's load, not only what it saves the peer.** A design that makes
core 0 the bottleneck defeats what spreading buys.

**CB8. Make the write unlogged, under CR6.** `RecordAccess`
(`catalog.cpp:2816`) today takes `OverwriteLogged(wal_, …)` on the
update path and `InsertRow(wal_, …)` on the insert path. Both lose their
WAL records under CR6.

**What that costs, to be established and not assumed.** An unlogged
catalog page has no redo and no undo, so recovery neither replays nor
rolls it back. The precedent is the free map, which is unlogged and
repaired at mount (D9, RC04) — **so the question CB8 has to answer is
what plays RC04's part here.** Three sub-questions, in order:

1. A torn or stale page 11 after a crash: is it detected, and by what?
   The page carries the common header's checksum; establish whether the
   mount path checks it for this page and what it does on failure.
2. **[PROPOSAL]** If a repair is needed, CLA proposes *discard* rather
   than repair — reset the relation to empty at mount. Invariant 8
   already prices a deleted trail as performance and never a result, and
   a discarded trail costs the shadow planner nothing it does not
   already tolerate (a relation with no rows is simply unweighed,
   `physical-optimizer.md` §5's decay score).
3. Whether an unlogged page can sit in a heap chain alongside logged
   ones without breaking anything that walks the chain. **Read this from
   source; do not reason it out.** C1-1 — the open defect where a failed
   grow leaves a heap chain non-ascending — is a live reminder that this
   chain's invariants are not fully understood, and page 11's chain is
   exactly the kind that grows.

**What CB8 does not fix**, stated so it is not read as fixed: a dropped
relation's `sys.access_stats` rows still ghost forever (DT4's decision)
and still consume the instance-wide `kMaxAccessShapes` (4,096,
`rows.hpp:681`). **[PROPOSAL]** CLA proposes leaving the cap
instance-wide and the defect open, where `known-gaps.md` already owns
it: under CR7 the relation is not per core, so a per-core cap has no
meaning, and the alternative — sweeping ghosts — is reclamation work
that A5's gate 3 blocks anyway.

---

## CB9 — the rule CR6 requires

CR6 is an exception to RV3, and the operator's direction was to add it
as a rule rather than leave it local. Write it into `docs/rules/rules.md`
§5 (On-Disk Format Rules), in the form the surrounding rules take:

> A catalog relation whose content is **only** a statistic — a trail
> invariant 8 already prices as performance and never a result — is
> written unlogged. It is neither redone nor undone, and a mount that
> finds it damaged discards it. This is the sole exception to RV3's rule
> that catalog writes are WAL-logged as ordinary record types
> (`ddl-transactional.md` §7). `sys.access_stats` is the only relation
> that qualifies today; adding a second requires showing it meets the
> same test, not that it resembles this one.

Amend `ddl-transactional.md` §7 to point at the exception, so RV3's own
text does not read as absolute.

---

## Where this lands

- `blueprint-range-ownership.md` §8 — CR4's table, `[OPEN]` removed.
- `blueprint-range-ownership.md` §11 — R1's row becomes the ratification.
  **R5's gate must be restated**: it reads "R1, R3", which after CR4
  names nothing buildable. What R5 needs is CB7's peer-recorded
  statistics, which is `crosscore.md`:475's stated prerequisite and
  which CR7 satisfies through core 0 with per-core attribution.
- `crosscore.md` §2 — the statistics item points at CB7 for its
  mechanism, and records that per-core *relations* were not the route
  taken.
- `known-gaps.md` — the M5 asymmetry entry moves from pending to
  decided; the `PeerDdlRefused` entry is amended by CR5.
- `docs/rules/rules.md` §5 — CB9.
- `well_known.hpp` — CB3.

## What this order does not claim

CB does not open R5. It satisfies one named prerequisite and leaves the
rest: split/migrate policy and its constants are `[OPEN]` and belong to
the physical optimizer's Part III, undrafted; merge is `[OPEN]`; D1
keeps every btree relation unsplittable, so a mover would move heap
relations only; and nothing reclaims a page, so a migration that frees
pages frees them into a supply that never reuses them.

R1 as §8 wrote it is closed by CR4's decline, not by this build.
