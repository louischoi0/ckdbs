# Ratification CR — catalog page placement, and the decline of §8

Ratified by the operator 2026-08-31 against `main` at `6b5b368`
(`v2.2.1-142-g6b5b368`). Recorded by CLA; the operator's wording governs
where this document paraphrases.

This supersedes the scope of `instructions/v2.7.0/r1-every-core-equivalent.md`,
which is withdrawn: its B0-B4 measurement series answered a question CR4
removes. **That order is marked closed and rejected** (operator,
2026-08-31); the file is kept as a tombstone carrying the disposition, so
this citation resolves to a decision rather than to nothing.

## CR1 — a catalog relation's root page stays reserved; its var-heap does not

The bootstrap catalog heap roots (`kCatalogPageTypes = 4` through
`kCatalogPageRanges = 15`, `well_known.hpp:271-299`) stay below
`kFirstUserPageId = 128`, for the reason those constants already state:
they must be findable at bootstrap without a catalog read.

**Their var-heap pages move out of the reserved range by decision, not by
accident.** Today `sys.pattern_defs` and `sys.assertions` already allocate
their var-heap roots through `CreateNew()` and record them in `sys.tables`
(`well_known.hpp:277-282, 289-293`). CR1 ratifies that placement as
intended rather than incidental, and makes it the rule for any catalog
relation that gains a var-heap.

**What this costs, stated rather than discovered later.** A page outside
the reserved range is not readable by a peer through
`MayFault`'s `page_id < system_page_limit_` arm
(`device_page_store.cpp:656`). Today the only peer-readable catalog
var-heap is `sys.assertions`, reached by granting the individual pages a
row names (`exec::AssertionSpillPages`) — deliberately not an extent
grant, because an extent would cover pages that core owns and cost it the
stamp-claimed write rights of PW1c-7. CR1 does **not** ratify the
page-at-a-time workaround as the general mechanism; CR3 is what governs.
`sys.pattern_defs` has the same shape and no peer reader today, so it is
the first place CR3 will be exercised.

## CR2 — DDL executes on core 0; a peer sends and waits

A peer does not execute DDL. It sends the request to core 0 and waits for
the outcome. PW4's refusal (`command_dispatcher.cpp`'s `PeerDdlRefused`)
therefore **stays necessary** and is not retired.

This is the deliberate opposite of `blueprint-range-ownership.md` §8's
second item, whose wording was that the refusal "becomes unnecessary
rather than unbuilt". §8 wanted the refusal to stop existing; CR2 keeps it
and gives DDL a route around it. The item is declined, not satisfied, and
CR4 records that.

Two things this does not decide, flagged as open rather than assumed:

- **The wire form.** Statement shipping exists for DML
  (`session_step_client`/`remote_step_service`); whether DDL rides the
  same path or gets its own request kind is unsettled here.
- **The reply's failure mode.** A shipped read whose reply exceeds 992
  bytes reports `UNKNOWN_OUTCOME` today, which `known-gaps.md` already
  records as the wrong thing to tell a client about a statement with no
  effect. A DDL reply is small, but a DDL *error message* carries a byte
  position and a relation name and is not obviously bounded. Size the
  reply before building, do not assume it fits.

## CR3 — catalog pages may leave the reserved range once grown

Catalog pages are allocated and initialised inside the reserved range at
bootstrap. Beyond bootstrap, a catalog page **may be managed outside the
reserved range** in either of two cases:

1. the relation grows past what the reserved range holds, or
2. every peer must read and write the page equally.

Such a page takes the ordinary relation rules — allocation from the
general supply, free-map accounting, extent leases, WAL logging as RV3
already established for catalog changes. This is the "partial general
relation rule" the operator's wording names.

**The consequence CR3 is chosen for.** `sys.access_stats` is pinned at
`kCatalogPageAccessStats = 11`, inside the reserved range, so under CR2's
core-0-only write rule a peer could not record an access at all —
and `RecordAccess` (`catalog.cpp:2816`) runs per statement, not per DDL.
CR3 is what allows a per-core `sys.access_stats` to sit in general page
space, owned and written by its own core, which is
`crosscore.md`:475's prerequisite for the mover (R5).

**Unresolved and owed by whoever builds this.** The reserved-range roots
are justified by bootstrap findability. A page that migrates out of the
range is found through `sys.tables` instead — which is the mechanism
`varheap_page_id` already uses and which `rows.hpp` calls DDL-immutable
and therefore cacheable. Whether that indirection holds for a *heap* page
rather than a var-heap root is not established here. Establish it from
source before the first relation is moved.

## CR4 — `blueprint-range-ownership.md` §8 is declined as written

§8 ("Every core equivalent — retiring M5") is declined. M5 stands: core 0
is the sole writer of the reserved range, and every core reads it.

Item by item, so that a later reader does not have to reconstruct which
part survived:

| §8 item | Disposition |
|---|---|
| 1. Superblock, free map and catalog take a partition-boundary lock each, or stay message-serialised | **Moot.** One writer, no contention. The `[OPEN]` marker is removed, not resolved — and with it the unsettled question of whether `rules.md` §3's partition-boundary allowance is an exception to guideline 1, which no document answers and which this decision no longer needs answered |
| 2. DDL runs on any core; PW4 becomes unnecessary | **Declined.** CR2 |
| 3. Per-core listeners become the front door | **Partial, by consequence.** Reads are already equal (`MayFault`, `well_known.hpp:189` calls it a correctness requirement); writes ship to core 0. No further work is opened by this ratification |
| 4. Statistics relations become per-core | **Preserved and unblocked by CR3** — see the reading note below |

**Reading note, requiring the operator's confirmation.** The operator's
fourth decision was written as "항목 8 기각". §8 has four items and no
item 8, so CLA has read it as *§8 itself is declined*, which is the
reading consistent with CR1-CR3 — those three replace what §8 set out to
do. Two other readings exist and CLA cannot rule them out from the text:
that item 2 is meant (already covered by CR2, so redundant), or that item
**4** is meant. **If item 4 is what is declined, R5 does not open**:
`crosscore.md`:475 states that a peer recording nothing cannot feed the
mover and that per-core statistics are a prerequisite of §7 rather than an
optimisation. Confirm before this table is treated as settled.

## What this ratification leaves standing

Three things a reader might expect to be closed here and which are not.

**The M5 asymmetry is now permanent by decision.** Core 0 bumps
`sys.tables.next_id` directly while a peer takes a `RowIdLeaseTable`
grant; `known-gaps.md` recorded this as "blueprint R1's to retire". It is
not retired — it is kept. One measured consequence follows and does not
go away: under `placement = rotate`, a relation owned by a peer spreads
across the peers only, so a k-core cell is really (k-1)-way.

**Stale catalog reads remain.** A peer reads catalog bytes off the device,
so a page core 0 holds dirty is stale to that peer. CR2 makes the "one
writer per catalog page" discipline permanent, which is the safe
direction, but the staleness itself is untouched.

**`sys.ranges` write path is unchanged** — core 0 only
(`workplan-range-directory.md:1187`), consistent with CR2 and not
separately decided here.

## Where this lands

- `blueprint-range-ownership.md` §8 — replace the section body with CR4's
  table and a pointer here; remove the `[OPEN]`.
- `blueprint-range-ownership.md` §11 — R1's row becomes this document.
  **R5's gate must be restated**: it currently reads "R1, R3", which after
  CR4 no longer names anything buildable. What R5 actually needs is CR3's
  per-core `sys.access_stats`.
- `well_known.hpp` — CR1 as a comment beside the var-heap notes at 277 and
  289, so the placement reads as intended rather than incidental.
- `crosscore.md` §2 — the statistics item points at CR3 for its mechanism.
- `known-gaps.md` — the M5 asymmetry entry is amended from pending to
  decided.
- `docs/rules/rules.md` §3 — unchanged, and deliberately so: CR4 removes
  this project's only live candidate for a partition-boundary lock, but
  does not rule on what the term means. It stays undefined, with
  `wal/writer.hpp:27` its one applied instance.
