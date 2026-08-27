# T3 — the sorted heap fill (bulkinsert.md §8, v1)

Decisions T3-1..T3-6 resolve §8's prerequisites for a v1 that violates no
standing rule; tasks TS01-TS04. **v1 is a heap sorted fill inside the T1
statement**, not a new surface: it engages automatically when legal and
falls back to the row loop otherwise, with byte-identical replies and
relation state either way — the equivalence test is the feature's spine.

- **T3-1, scope**: heap-clustered relations only. The btree bottom-up
  build stays reserved — the bench put the bulk story on heap, and a
  btree insert is already a descent, not a walk.
- **T3-2, the gate (prerequisites 4 and 5, resolved conservatively)**:
  engages iff the statement is multi-row AND **every row omits its primary
  key** AND the relation is heap AND it carries no index, no Cabin, no
  assertion, AND its schema cannot spill (`SortedFillEligible`,
  `src/server/command_dispatcher.cpp`). FK stays allowed: forward checks
  run per row *before* any placement, so order is preserved and a refusal
  burns nothing — not even the range. Anything outside the gate takes the
  row loop; the gate can only widen.
  The key conjunct was added 2026-08-11 as `key_mode == kAssigned` and
  **became per-statement on 2026-08-25** with the mode's removal
  (`docs/spec/heap-and-tuple.md` §4.1). The reason is unchanged and is why it
  was never inherited from "heap": this path's whole shape — one
  contiguous id range carved up front, appended in order — leaves no place
  for a key the caller named. What changed is only *where the fact lives*:
  a relation used to declare it, and a row states it now, so the check
  moved to the call site and `SortedFillEligible` keeps the
  relation-shaped half. Ineligibility, never a refusal — a statement that
  names keys runs through the row loop.
- **T3-3, the id range (prerequisite 1)**: `AllocateRowIdRange(oid, n)` —
  one catalog write bumps `next_id` by n; issuance inside the range is
  monotone by construction. An aborted statement burns the whole range:
  K3 calls a burned id free, and BI9 already accepted the class.
- **T3-4, WAL is images, not a new record type (prerequisite 2, resolved
  without waiting for recovery)**: every page the fill touched logs as one
  `kFullPageImage` — the chain-growth and index-split precedent: only an
  image describes a page assembled off the per-row path. At ~150 rows a
  page, one image is *smaller* than the per-row records it replaces. No
  format event; replay semantics are the FPI's existing ones.
- **T3-5, the epoch (prerequisite 3)**: new pages carry `relayout_epoch`
  0 and no trail can name them — page ids are never reused — and the
  pre-existing tail mutates in place under the normal rules. Nothing to
  bump; the bottom-up *tree* build this mattered for is out (T3-1).
- **T3-6, per-row facts survive**: `NoteInsert` per row (rollback replays
  the trail), `min_key` set once per new page from the sorted stream
  (invariant 3's best case, exactly §8's promise), and the tail page's
  `id >= min_key` boundary check stays. The intra-range duplicate check
  is vacuous (engine-issued, contiguous) and the tail-page check against
  pre-existing rows is kept.

## TS01-TS04  **[DONE 2026-08-10]**  ## TS05 — Bench  **[DONE 2026-08-10]**

All built and measured the same day (`bench/results-bulk-insert.md` Part
III): batch-1000 relaxed 674,570 rows/s, 2.50x over the row-loop twin and
7.1x PostgreSQL; the placement slice fell 7.7x to 0.31 us/row and WAL to
74 B/row. Two findings worth carrying: **T3 batches should be sized in
pages** - below ~rows-per-page per statement the shared tail re-images
and the WAL economy halves - and **the residue is the parse (72%)**, so
T2 is now a ~3.5x proposition and the pipeline has nothing left worth a
tier.
