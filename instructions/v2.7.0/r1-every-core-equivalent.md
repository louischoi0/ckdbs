# Work order R1 — every core equivalent — **CLOSED, REJECTED**

**Status: closed and rejected, 2026-08-31, by the operator.** Nothing in this
order is to be built, measured or revived. It is kept as a tombstone rather
than deleted because two live documents cite it by name — the ratification
`r1-catalog-placement-ratification.md` and the build order
`cb-catalog-placement-buildout.md` — and a citation that resolves to nothing
reads as a lost file rather than as a decision.

## What it was

The order that would have executed `blueprint-range-ownership.md` §8:
retiring M5, making every core equivalent over the superblock, the free map
and the catalog, with a B0-B4 measurement series to choose between §8's two
candidates — a partition-boundary lock per structure, or a rotating
coordinator.

## Why it is rejected rather than finished

**Its measurement series answered a question that no longer exists.** B0-B4
existed to decide *how* several cores would write the shared structures. The
operator's ruling of 2026-08-30 (`crosscore.md` **CC11**) settled that one
core writes them and every core reads, declining both candidates — so there
was nothing left for the series to choose between, and the premise it would
have measured on (read scalability) had already fallen: a peer fills from
the device, not from core 0.

**And §8 itself was then declined** (2026-08-31, CR4). M5 stands. What
replaced the order is three rulings and one build:

- **CC12** (CR1-CR3) — catalog page placement: a relation's root page stays
  in the reserved range, its var-heap does not, and a grown catalog page may
  leave the range on the ordinary relation rules.
- **CC13** (CR5-CR8) — DDL's route (a peer ships to core 0 and waits, so
  `PeerDdlRefused` stays) and how a peer's statistics reach core 0 (folded
  locally, flushed on the tick, unlogged, dropped on a full ring).
- The build: `instructions/v2.7.0/cb-catalog-placement-buildout.md`, landed
  as CB0-CB9 (`docs/inflight/in-progress/workplan-catalog-placement.md`).

## What a later reader should not conclude from this rejection

That "every core equivalent" was tried and failed. It was **not built and
not measured** — it was overtaken by a decision that made its central
question moot. The one item of §8 that survived as a requirement is per-core
access statistics, and it was met by a different mechanism than this order
assumed: CR7's batching to core 0, **not** per-core statistics relations,
which the ratification declined along with the rest. That requirement now
sits where it belongs, as R5's gate in `blueprint-range-ownership.md` §11.

Reviving anything here needs a reason other than 2PC or scalability, since
those are the two the record already answers.
