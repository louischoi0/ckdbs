# Workplan — The Undo Purge (§9's retention decision, then the sweep)

Status: **ratified 2026-08-19** — D1 horizon-only, D2 undo-internal
recycle (free list in-memory for v1), D3 purge-on-growth, each the
recommended option; the alternatives below stand as the record of what
was declined and why. `docs/spec/txn.md` §9's undo-retention entry closes
against this file when the build lands. The prerequisite fell with
`docs/workplan-reader-registration.md`: `TransactionManager::ReadHorizon()`
exists, per-core, and the catalog delete-mark purge is its proven first
consumer.

## The one soundness fact everything below leans on

An undo record holds the version its writer **replaced** (a
before-image). A reader walks into undo only while writers are invisible,
and stops at the first visible one — so a record is reachable only by a
view that cannot see its writer. Therefore:

> **A record whose writer is below `ReadHorizon()` is unreachable by
> every live and future traversal.**

(The near-miss to avoid re-deriving: it is tempting to think a record
stays needed while its *superseder* is above the horizon. Wrong way
round — the version an old writer wrote lives in its *superseder's*
record, not its own. The record's own reachability is decided by its own
writer's visibility, exactly parallel to §5d's mark rule.)

Recovery is not an exception: its undo phase walks the
`txn_prev_undo_ptr` chains of transactions that were **active** at the
crash, and an active transaction bounds the horizon at or below its own
id — so no record recovery could ever need is purgeable while its writer
runs.

The purge unit is a **whole page**: records are addressed only by byte
offset (no slot directory), so nothing smaller can be reclaimed. A page
is freeable when the maximum writer id over its records is below the
horizon.

## D1 — Retention policy `[OPEN — ratify]`

**(a) Horizon-only. RECOMMENDED for v1.** A page is freeable exactly when
the horizon has passed its newest writer; nothing else is ever freed.
`SnapshotTooOld` stays structurally unreachable — the same contract the
engine has today, now with reclamation. Cost: one long-running
transaction or leaked lease blocks reclamation for its lifetime (undo
grows meanwhile, exactly as it does today for *every* lifetime). No new
error surface, no new config key, smallest correct step; a cap can be
added later without unbuilding anything.

**(b) Horizon plus a byte cap.** Retain at most `undo_retention_bytes`;
past the cap, purge ignores stragglers' horizons and old readers get
`SnapshotTooOld` on a traversal into freed space. This is what makes the
error *reachable*, and it needs everything (a) does not: detecting a
dangling traversal (a generation stamp per recycled page), the error's
class and coded spelling (`retryable=1` for autocommit — a retry mints a
fresh view; fatal for an open RR transaction), and a config key with a
default nobody can currently derive. Oracle's shape, and Oracle's
ORA-01555 with it.

**(c) Time-based retention.** A clock where (b) has a byte count; same
machinery, worse bound (idle time retains garbage, busy time may retain
too little). Listed for completeness, recommended never.

## D2 — What "free" means `[OPEN — ratify]`

**(i) An undo-internal recycle list. RECOMMENDED.** A freed page returns
to the undo log's own free list and the next chain growth reuses it
(`FormatUndoPage` again — `PAGE_INIT{kUndo}` is an existing record type
recovery already applies). This dodges the engine-wide page-reuse gate
(`physical-optimizer.md` §6 gate 3 is about *heap* pages and trail
validation; no advisory structure points into undo) and stale
`undo_ptr`s are harmless by the soundness fact above — no valid
traversal dereferences into a purged record. Undo stops *growing*;
its footprint high-water-marks instead.

**(ii) Return pages to a global free map.** Blocked: no free map exists
and gate 3 owns the question. Not this workplan's fight.

Sub-decision for (i), the free list's persistence: **in-memory for v1**
(a crash forgets it and leaks those pages until they are re-found — a
later mount-time scan can rebuild it from `kUndo` pages' emptiness, the
`nr_records` field exists precisely to make that scan cheap). Logging
the list would touch `wal.md` §4.1 for a bookkeeping structure that is
reconstructible, which is the FPI-exemption argument in reverse.

## D3 — Cadence `[OPEN — ratify]`

**(A) Purge-on-growth. RECOMMENDED.** When the undo log is about to
chain a new page, it first attempts to reclaim from the tail: walk the
`prev_page_id` chain oldest-first, free while pages pass the horizon
test, stop at the first that fails. Amortized O(1) per growth, zero new
scheduling machinery, work happens exactly when the resource is being
consumed, and a workload that writes no undo pays nothing. The stop-at-
first-failure is conservative (append order is not id order — an old
transaction can write into a new page) and is what bounds the pass.

**(B) A `maintenance`-group tick.** What `txn.md` §9 sketched. Needs a
cadence decision, a scheduler seam, and an idle-cost argument — all
solvable, none needed while (A) covers the growth path. Add later if
measurement shows tail latency on the growth path.

**(C) On-commit.** Hot-path work for a cold-path resource. No.

## The 48-bit problem `reserved1` cannot hold `[decided by D1-D3 choice]`

The header reserved `reserved1` (32 bits) for a per-page high-water
mark, but writer ids are 48-bit. Options, cheapest first: compute the
page's max writer at sweep time by decoding its records (the sweep
visits a page rarely and the codec exists — **recommended**, no format
change); widen the header (an on-disk layout change for a derivable
number); store a truncated epoch (invites exactly the wraparound
class §9 refuses elsewhere). With sweep-time computation, `reserved1`
stays reserved and `nr_records == 0` remains the fast skip.

## Task sketch (firm after ratification)

- **UP1** — ratify D1-D3 here and in `txn.md` §9; this file becomes the
  workplan. ☑
- **UP2** — the sweep. ☑ — with one design correction found in survey:
  the per-page bound is **kept at append time in memory**
  (`UndoLog::TrackedPage`), not computed by decoding records at sweep
  time as sketched above, because **a record does not store its own
  writer's id** — it stores the superseded writer's. The 48-bit section
  below fell away with it: no header field is needed at all for the
  live chain, since a restart abandons it anyway. `reserved1` stays
  reserved; the on-disk `prev_page_id` chain becomes historical once
  reuse starts (its only reader, `PageCount()`, now counts the
  in-memory chain).
- **UP3** — the trigger. ☑ — growth reclaims then prefers the recycle
  list; the manager arms the horizon source at construction and disarms
  at destruction; `SHOW META` prints `undo_pages_live` /
  `undo_pages_recycled`. A bare log (no manager) never frees, which is
  what keeps every pre-existing socket-free test byte-identical.
- **UP4** — **deferred by the ratified D2 sub-decision**: the recycle
  list is memory-resident, a previous run's pages leak exactly as they
  always have, and `known-gaps.md` states it. The mount-time rebuild
  (`nr_records` exists to make its scan cheap) is the open remainder.
- **UP5** — ck-tester. ☑ **Measured 2026-08-19** (Release, interleaved,
  `d479f1f` vs base `a0c18cd`; raw JSON in the session scratchpad, no
  `bench/` file by the one-document rule):
  - **Per-statement: nothing.** Autocommit pk UPDATE p50 deltas of
    −0.2/+0.1/+0.1 µs at 200/1k/10k rows against a 0.2–0.5 µs in-run
    floor; p99 deltas flip sign across sizes, so the growth arm (one
    reclaim walk + reuse per ~119 statements — exactly p99 territory)
    resolves to nothing either.
  - **Footprint: total.** Over 30k UPDATEs, HEAD held `undo_pages_live`
    at exactly 2 through all 16 samples with `undo_pages_recycled`
    climbing 1 per ~119, and the data file **byte-for-byte flat** at
    1,572,864; the base grew +2.0 MiB (256 pages, one per ~117
    updates, ~67 MiB per million updates, unbounded in loop length).
    Same growth demand on both sides (252 recycles + 2 ≈ 256 new
    pages); opposite disposition.
  - **The caveat that must travel with the plateau number**: with no
    concurrent readers the steady state is a two-page ping-pong **by
    construction** — every writer clears the horizon immediately. A
    long-running reader grows `live` for its lifetime, which is D1's
    accepted cost, not a defect the run could see.
  - PostgreSQL twin skipped by decision: the analogue is VACUUM, a
    different mechanism on a different schedule; no shared definition.
- **UP6** — docs: `txn.md` §3/§4.1/§9, `undo_log.hpp`'s header,
  `undo_page.hpp`'s field notes, `known-gaps.md`'s reclamation section,
  `CLAUDE.md` row and Open Decisions index. ☑

## Not in scope whatever is ratified

Heap tuple-slot reclamation, var-heap reclamation, index/Cabin entry
reclamation, page compaction — each has its own owner and its own gate;
this workplan only makes the undo log stop growing without bound.
