# Physical Optimizer — Statistics-Driven Relayout (v1: Shadow)

Status: **ADOPTED (v1 scope, 2026-08-09); built through PX06 the same day** —
the decay score, the epoch with real comparisons, the planner and
`SHOW RELAYOUT` all exist; PX07's shadow measurement is in flight and PX08's
close-out is done. Every `[PROPOSED]` below was built as proposed.
Markers: `[CONFIRMED]` is settled,
`[PROPOSED]` is a default to amend before building, `[OPEN]` must not be
assumed. Decisions are numbered `R1`-`R12`.
Related documents: `heap-and-tuple.md` §7 (the normative relayout section
this file expands), `rule-fixed-length-tuple.md` (tuple mobility),
`spec-eviction.md` (EV1's temperature hook), `spec-pattern-tracking-levels.md`
(decay-ranked trail eviction), `waystone-concpets.md` §3.1 (epoch validation),
`feat-cabin.md` (relocation invariance), `feat-index.md` (IX3),
`txn.md` §9 (no reader registration), `parser-v2.md` I7/V11.
Task breakdown: `docs/workplan-physical-optimizer.md` (`PX01`-`PX08`).

**On the numbering.** Two shipped specs already cite "the physical-optimizer
lazy-decay score (R1)" from a blueprint that does not exist in this
repository — `spec-eviction.md` EV1 and `spec-pattern-tracking-levels.md` §3
both lean on it, and `rule-fixed-length-tuple.md`'s status line claims
consistency with it. This document backfills that blueprint, and **R1 is
assigned to the lazy-decay score** so the existing citations become true
rather than corrected. Note `parser-v2.md` I15 also names an "R1" (the
no-fetch-under-span rule); cite the file with the number, the same rule the
three `P`-numbered workplans already forced.

---

## 1. Positioning

Engine-driven physical optimization is one of the two things this project
exists for (`heap-and-tuple.md` §1), and it has been running with its input
half only: collection landed 2026-08-03 (`sys.access_stats`, the
`kFilterScan` split, the 2026-08-08 index-kind split), and nothing consumes
it but `SHOW ACCESS`.

**v1 is shadow-only, and that is the finding, not a hedge.** The design work
for this spec audited every candidate move against the engine as it stands,
and each one is blocked by a named gate (§6): compaction needs the reader
horizon that deliberate non-registration withholds, hot clustering breaks the
ordered-between property `kRange` pruning reads, and retiring a page for
reuse breaks trail validation across relations. A mover shipped today would
either be incorrect or would move bytes no query benefits from. So v1 applies
the optimizer's own promotion-gate philosophy (`overview.md` §4) to the
optimizer itself: **observe, classify, plan, report — enact only when a gate
opens, with the shadow report as the evidence that opening it pays.**

What v1 ships is real regardless:

1. **The lazy-decay score (R1)** — the one time-decay implementation three
   subsystems have been promised.
2. **The page epoch (R4)** — the field, its discipline, and real reads at
   Waystone's and the Cabin's validation sites. Two subsystems are explicitly
   "waiting on the epoch that must land with relayout"; this spec *is*
   relayout arriving, so the epoch lands here.
3. **The planner and `SHOW RELAYOUT` (R9, R10)** — the physical health
   report: what would be done, what it would buy, and which gate blocks it.

The mover is specified structurally (§4's legal-move table, R6, R7) so that
building it later is filling in a form, not reopening the design — but no
mover code ships in v1 (R11).

---

## 2. Decision Record

| ID | Decision |
|----|----------|
| R1 | **The lazy-decay score.** Exponential half-life decay computed lazily from a stored `{score, last_bump}` pair: a touch decays-then-increments, a read decays only, and there is no background decay pass — idle data costs nothing and is never visited. One implementation (`include/kds/stats/decay.hpp`), `sched::Clock`-injected; with no clock the score degrades to a raw count, the same best-effort stance `sys.access_stats.last_seen` already takes. Half-life is the `decay_half_life` config key, per instance, default 600 s `[PROPOSED]`. Declared consumers: hot/cold classification here, trail-retention ordering (`spec-pattern-tracking-levels.md`), and EV1's experimental temperature hook. Scores are memory-resident and never persisted `[PROPOSED]`. |
| R2 | **Inputs are the existing collectors only**: `sys.access_stats` (the shape axis), Waystone sightings (the value axis), and what a page itself says when walked. The optimizer adds no third collector; an input it lacks becomes a collection change in the layer that owns collection, spec'd there first. |
| R3 | **Two halves with a hard seam.** The **planner** is pure — it reads statistics and the catalog and produces `RelayoutPlan`s with predicted benefit — and the **mover** enacts plans. Shadow mode is the planner without the mover. The `physical_optimizer` config key takes `off | shadow` (default `shadow`); `on` is **refused at startup naming the open gates**, so a config written for the future fails loudly today instead of silently under-delivering. |
| R4 | **The page epoch** settles `heap-and-tuple.md` §3.1a's `[OPEN]`: `PageHeaderFields::reserved0` (offset 16, u64) becomes `relayout_epoch` — the field the header comment already nominated. Every existing page carries 0 there, so **no format bump**: a zero reads as epoch 0. Durable by construction (it is header bytes), which trails need because trail pages are durable. Bumped **only by the mover** when tuples move; INSERT/UPDATE/DELETE never bump, because the fixed-length rule makes them address-stable — that stability is the whole reason replay is safe today. Wraparound is unreachable at u64 width rather than handled. **Pairing rule: no consumer may accept a location on epoch equality alone** — the epoch is a fast whole-page invalidation layered over the Keystone-id check (K1), never a substitute for it. |
| R5 | **The legal-move table (§4)** is normative for any mover, v1 or later. It derives from invariants 2, 3, 4, 8, 14, from `kRange` pruning's ordered-between dependency, and from rollback's in-memory undo trail naming addresses. |
| R6 | **Mover execution context**: a maintenance-group task on the relation's home core, never cross-core. Safe today by run-to-completion; the moment the executor becomes suspendable it **must** gain a relation-busy guard (no in-flight statement holding a position on the relation, no open transaction whose undo trail names addresses in it) — the suspension-audit precedent: mechanical, debug-asserted, not remembered. |
| R7 | **Mover logging**: a full-page image of every page it mutates plus `PAGE_INIT` for pages it creates `[PROPOSED]`. A `HEAP_RELAYOUT` record type is reserved, not assigned — the FPI is the honest v1 shape for the same reason chain growth uses one. An unlogged relayout is forbidden even while recovery does not exist: the WAL-before-data gate is store-enforced, and a log that names slots a relayout silently moved is a log that lies. |
| R8 | **The maintenance surface is deliberately empty.** Cabins and secondary indexes are relocation-invariant (value = pk indirection, `feat-cabin.md`, `feat-index.md` B2); the var-heap is untouched (invariant 14); trails are invalidated by the epoch bump and self-heal on next execution. A **heap relation has no pk index**, so a heap-relation mover maintains *nothing but the epoch* — which is why the first mover targets heap relations (§4). `heap-and-tuple.md` §7's "keep the B+ tree consistent" applies only to btree-clustered relations, whose relayout is the tree's own restructure and out of v1's scope entirely; this spec amends that parenthetical. |
| R9 | **The benefit model**: predicted benefit = pages-not-touched per execution × decayed shape frequency, reported per plan in pages and per shape. The promotion metric — measured-after against predicted — becomes computable only when a mover exists, and the planner's output format carries both fields from day one so the comparison needs no format change. |
| R10 | **The v1 planner is pull-only**: computed when `SHOW RELAYOUT` asks, no background task, no cadence. Zero idle cost, no timing-wheel dependency (`sched.md`'s wheel is unbuilt), and the cadence decision lands with the mover, which is what actually needs one. |
| R11 | **v1 scope**: R1 + R4 + the planner and report. No mover. Every enactment is blocked by a named gate (§6) and the report says which. |
| R12 | **Per-relation gate**: a mover consults `parser-v2.md` V11's `WITH (PHYSICAL_OPTIMIZER = ON\|OFF)` catalog flag once both exist. The v1 planner reports every relation regardless — a report is free of risk, and an operator who opted a relation out still wants to see what that declines. |

---

## 3. The lazy-decay score (R1)

The stored state is two words: `score` and `last_bump` (a `sched::Clock`
reading). The decayed value at time *t* is
`score · 2^(-(t - last_bump) / half_life)`. A touch computes the decayed
value, adds 1, and stores `{decayed + 1, t}`; a read computes and does not
store. "Lazy" is the design, not an optimization: there is no sweep, so a
structure holding ten thousand cold scores pays nothing for their coldness
until something asks.

Rules:

- **One implementation.** `include/kds/stats/decay.hpp`, pure functions over
  the pair, `Clock`-injected like every time consumer (`sched/clock.hpp`'s
  contract). A second decay formula anywhere is the same defect as a second
  literal-coercion path was (`spec-types.md` §3.1).
- **No clock, no decay**: the score degrades to a raw counter. Deterministic
  tests inject `ManualClock` and get exact halving.
- Fixed-point arithmetic `[PROPOSED]`: u32 score scaled by 256, shift/mask
  only, no floating point on any statement path (`rules.md`).
- The half-life is one instance-wide config key. Per-consumer half-lives are
  a decision nothing yet motivates; if one arrives it is a new key, not a
  parameter that silently forks the meaning of "hot".

---

## 4. The legal-move table (R5) — normative for any mover

A mover, whenever one is built, operates under exactly these rules. They are
written now so the gates of §6 have precise shapes to open against.

**May:**

- Move a live tuple's bytes **verbatim** — Keystone word, MVCC header
  (`trx_id`, `undo_ptr`, `data_len`, flags), cells — to a slot on another
  heap page of the same relation. A move is not a version: no undo record,
  no visibility change, and readers that arrive through the undo chain are
  untouched because undo is reached *from* the tuple, never the reverse.
- Create new pages, choosing each page's `min_key` at format time, and
  retire whole source pages. Re-partitioning is **new-pages-then-retire,
  never an in-place boundary edit** — `min_key` is immutable (invariant 2).
- Move delete-marked tuples along with live ones. Dropping one is
  compaction, and compaction is gated (§6, gate 1).

**Must:**

- Keep invariant 3 at every intermediate state, not just at the end: no
  tuple sits in a page whose `min_key` exceeds its id, even transiently.
- Keep the chain **ordered-between**: `min_key` nondecreasing along
  `next_page_id`, every page's ids at or above its own `min_key`. `kRange`'s
  tail pruning (`VisitControl::kStop` at the first page past the high bound)
  reads this property; a mover that breaks it turns pruning into row loss.
- Bump `relayout_epoch` on every source and destination page, under the same
  exclusive access as the move, before any statement path can observe the
  new layout.
- Log per R7, run per R6.
- Refuse to run while any open transaction holds an in-memory undo trail
  naming addresses in the relation — rollback replays recorded
  `(page_id, slot)` writes, and a move underneath it would land the
  compensation on the wrong tuple.

**Must not:**

- Touch a `kVarHeap` page (invariant 14), any catalog page, any undo page,
  any trail, Cabin, or index structure. R8 is the point: the mover's entire
  maintenance surface is the epoch.
- Target a btree-clustered relation in v1 `[PROPOSED]`: a btree leaf is a
  heap page, so "relayout" there is a tree restructure with descent
  consistency to preserve — a different feature. The first mover targets
  heap relations, where R8 leaves nothing to maintain.
- Return a retired page to the free map (§6, gate 3). Retired pages are
  **quarantined** — held out of every allocator — until cross-relation reuse
  is made detectable. A quarantine leaks; the leak is the honest price and
  is bounded by how much relayout runs.

---

## 5. The planner and `SHOW RELAYOUT` (R9, R10)

`SHOW RELAYOUT` (all relations) reads `sys.access_stats` and the catalog
only. `SHOW RELAYOUT <relation>` may additionally walk that relation
read-only — ordinary visitor, stoppable, budget-charged — to measure what
statistics cannot: delete-mark density and per-page live fill. The walk is
priced by the caller having asked; the all-relations form never walks.

Per relation the report carries:

- the shape summary: each `(kind, columns)` row with raw `use_count` and its
  R1-decayed weight;
- chain length in pages, and (per-relation form) delete-marked tuples and
  the pages they would free;
- one line per **candidate plan kind**, each with predicted benefit (R9) and
  its verdict: `blocked_on=<gate>` in v1, always.

v1 names three plan kinds, none enactable:

| Plan kind | What it would do | Blocked on |
|---|---|---|
| `compact` | Rewrite the chain dropping delete-marked tuples past the reader horizon; reclaim emptied pages | Gate 1 |
| `cluster` | Co-locate a hot set on fewer pages | Gate 2 |
| `defrag` | Rewrite a chain onto contiguous page ids for sequential I/O | Gate 3 |

The report is the deliverable: it is what turns "should we open a gate" from
taste into a number, per relation, on a live workload.

---

## 6. The gates — why v1 enacts nothing

1. **Reader horizon.** Dropping a delete-marked tuple requires knowing no
   snapshot can still need it, and readers are deliberately unregistered
   (`txn.md` §9) — the same fact that makes purge impossible makes
   compaction impossible. Owner: the undo-retention open decision. This spec
   does not move it; a mover that guesses a horizon is the partial recovery
   `txn.md` §8 forbids, in different clothes.
2. **Ordered-between compatibility.** Clustering an arbitrary hot id set
   onto one page satisfies invariant 3 (set `min_key` to the set's minimum)
   while silently breaking the between-pages ordering `kRange` pruning
   reads. A legal clustering form needs one of: restriction to contiguous
   key ranges (which today's full pages make a no-op), a per-relation
   pruning opt-out, or a page-level "unordered" mark pruning respects. All
   three are `[OPEN]`; the shadow report's `cluster` lines are the evidence
   the choice should be made from.
3. **Cross-relation page reuse.** Trail validation checks `rel_oid` and the
   Keystone id at the recorded `(page_id, slot)` — but Keystone ids are
   issued **per relation**, so a page freed from relation A and reallocated
   to relation B can hold a colliding id at the recorded slot, and
   `PAGE_INIT` writes epoch 0, so the epoch check passes too. This is the
   second validation gap `waystone-concpets.md` §3.1 already names, made
   live the moment a mover frees a page. Owner: shared with free-map
   reclamation (open) — candidate fixes are a never-reset allocation epoch
   or a page-ownership check, and the quarantine rule (§4) is the stand-in.

Not a gate, but a standing constraint worth restating: raw page spans are
safe only because nothing evicts (`page.md` §3). A mover neither evicts nor
suspends under a span, so it adds no new exposure — but the `PageRef`
migration remains the prerequisite for *eviction*, and nothing here changes
that sequencing.

---

## 7. The epoch lands here (R4) — *built, PX03/PX04, 2026-08-09*

- `relayout_epoch` is read and written through `page_header.hpp` accessors;
  `PAGE_INIT` and every page-format path leave it 0. No format bump: every
  existing page already reads 0.
- **Waystone**: the recorder stores the page's current epoch in the trail
  entry instead of the literal 0 (`trail_store.hpp`'s documented gap);
  replay compares recorded against current, and a mismatch is a per-entry
  miss with the ordinary fall-through. Rule 2 of §3.1 stops being
  unenforceable.
- **Cabin**: `CabinEntry::page_epoch` is recorded from the header and
  compared in `exec/tuple_verify.hpp` — the one shared verifier, so Waystone
  and Cabin gain the real check at one site.
- Until a mover exists every comparison is between two zeros — **the check
  is real and its inputs are constant**, which is exactly the state the
  contract tests must pin: a test that hand-bumps a page's epoch must see
  replay miss, heal, and answer byte-identically. That test is writable in
  v1 and is the proof the epoch actually guards something.

---

## 8. Config and surface

| Key / verb | Values | Default | Meaning |
|---|---|---|---|
| `physical_optimizer` | `off` / `shadow` | `shadow` | `off` makes `SHOW RELAYOUT` answer a one-line disabled notice; `on` is refused at startup naming §6's gates |
| `decay_half_life` | seconds, > 0 | 600 `[PROPOSED]` | R1's half-life, instance-wide |
| `SHOW RELAYOUT [<relation>]` | — | — | §5's report; the bare form never walks a relation |

Nothing is reserved: `relayout` is an ordinary identifier, statement
fingerprints do not move, and `kFingerprintVersion` stays where it is — the
golden corpus is the evidence, as always.

---

## 9. Required tests

- Decay unit tests under `ManualClock`: exact halving, touch-vs-read,
  no-clock degradation to a raw count.
- Epoch round-trip: field read/write, `PAGE_INIT` zeroing, and the
  no-format-bump claim (a pre-change page image mounts and reads epoch 0).
- The hand-bumped-epoch contract test, in both suites: Waystone replay and a
  Cabin resolve against a page whose epoch was bumped by the test must fall
  through per entry and answer byte-identically to the authoritative path.
- Planner: golden report over a seeded workload; the all-relations form
  provably performs no relation walk (page-fetch counter flat).
- The advisory family's standing rule, trivially satisfied and still
  asserted: `SHOW RELAYOUT` changes no query result.

---

## 10. Out of scope / later

- The mover, its cadence, and the first enacted plan kind — each behind a
  §6 gate, chosen from shadow data.
- Btree-clustered relation relayout (a tree restructure, not a chain
  rewrite).
- Temperature-unified eviction (EV1's experimental hook) — it consumes R1
  and decides nothing here.
- Score persistence, per-consumer half-lives, and any per-pattern hot-set
  clustering beyond what gate 2's resolution licenses.
