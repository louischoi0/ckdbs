# Physical Optimizer — Two Parts: Relayout (Part I) and the Cabin Controller (Part II)

**One umbrella, two halves, merged 2026-08-09.** Part I (this file through
§10) is **statistics-driven relayout** — shadow-only v1, decisions
`R1`-`R12`, tasks `PX01`-`PX08`, built through PX06. Part II (from the
divider after §10) is **autonomous advisory Cabin management** — the
`CABIN AUTO` promotion pipeline, decisions `PO1`-`PO10`, tasks
`PHY01`-`PHY08` (workplan Part II), **nothing built**. Part II consumes
Part I's R1 decay score and neither part touches the other's structures;
the merge note at the Part II divider carries the history.

Status (Part I): **ADOPTED (v1 scope, 2026-08-09); built and measured the
same day** — the decay score, the epoch with real comparisons, the planner
and `SHOW RELAYOUT` all exist; PX07's measurement
(`bench/results-physical-optimizer-shadow.md`) verified zero idle cost at
exact noise and priced the report at ~60 µs + 24 ns/slot. Every
`[PROPOSED]` below was built as proposed.
Markers: `[CONFIRMED]` is settled,
`[PROPOSED]` is a default to amend before building, `[OPEN]` must not be
assumed. Part I decisions are numbered `R1`-`R12`.
Related documents: `heap-and-tuple.md` §7 (the normative relayout section
this file expands), `rule-fixed-length-tuple.md` (tuple mobility),
`eviction.md` (EV1's temperature hook), `pattern-tracking-levels.md`
(decay-ranked trail eviction), `waystone-concpets.md` §3.1 (epoch validation),
`cabin.md` (relocation invariance), `index.md` (IX3),
`txn.md` §9 (no reader registration), `parser-v2.md` I7/V11.
Task breakdown: `docs/workplan-physical-optimizer.md` (`PX01`-`PX08`).

**On the numbering.** Two shipped specs already cite "the physical-optimizer
lazy-decay score (R1)" from a blueprint that does not exist in this
repository — `eviction.md` EV1 and `pattern-tracking-levels.md` §3
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
| R1 | **The lazy-decay score.** Exponential half-life decay computed lazily from a stored `{score, last_bump}` pair: a touch decays-then-increments, a read decays only, and there is no background decay pass — idle data costs nothing and is never visited. One implementation (`include/kds/stats/decay.hpp`), `sched::Clock`-injected; with no clock the score degrades to a raw count, the same best-effort stance `sys.access_stats.last_seen` already takes. Half-life is the `decay_half_life` config key, per instance, default 600 s `[PROPOSED]`. Declared consumers: hot/cold classification here, trail-retention ordering (`pattern-tracking-levels.md`), and EV1's experimental temperature hook. Scores are memory-resident and never persisted `[PROPOSED]`. **Two reads (2026-08-10):** the Q24.8 `ValueAt` — which underflows to zero after ~16 half-lives, so it ranks live data only — and `Log2ValueAt`, the same state read in the log domain, where decay is a subtraction and ordering survives indefinitely. The state did not change: the underflow was always in the read. Any consumer that must order *idle* things (an eviction victim, a retirement) uses the log read; see §II.4's note for what that fixed and what it structurally cannot. |
| R2 | **Inputs are the existing collectors only**: `sys.access_stats` (the shape axis), Waystone sightings (the value axis), and what a page itself says when walked. The optimizer adds no third collector; an input it lacks becomes a collection change in the layer that owns collection, spec'd there first. |
| R3 | **Two halves with a hard seam.** The **planner** is pure — it reads statistics and the catalog and produces `RelayoutPlan`s with predicted benefit — and the **mover** enacts plans. Shadow mode is the planner without the mover. The `physical_optimizer` config key takes `off | shadow` (default `shadow`); `on` is **refused at startup naming the open gates**, so a config written for the future fails loudly today instead of silently under-delivering. |
| R4 | **The page epoch** settles `heap-and-tuple.md` §3.1a's `[OPEN]`: `PageHeaderFields::reserved0` (offset 16, u64) becomes `relayout_epoch` — the field the header comment already nominated. Every existing page carries 0 there, so **no format bump**: a zero reads as epoch 0. Durable by construction (it is header bytes), which trails need because trail pages are durable. Bumped **only by the mover** when tuples move; INSERT/UPDATE/DELETE never bump, because the fixed-length rule makes them address-stable — that stability is the whole reason replay is safe today. Wraparound is unreachable at u64 width rather than handled. **Pairing rule: no consumer may accept a location on epoch equality alone** — the epoch is a fast whole-page invalidation layered over the Keystone-id check (K1), never a substitute for it. |
| R5 | **The legal-move table (§4)** is normative for any mover, v1 or later. It derives from invariants 2, 3, 4, 8, 14, from `kRange` pruning's ordered-between dependency, and from rollback's in-memory undo trail naming addresses. |
| R6 | **Mover execution context**: a maintenance-group task on the relation's home core, never cross-core. Safe today by run-to-completion; the moment the executor becomes suspendable it **must** gain a relation-busy guard (no in-flight statement holding a position on the relation, no open transaction whose undo trail names addresses in it) — the suspension-audit precedent: mechanical, debug-asserted, not remembered. |
| R7 | **Mover logging**: a full-page image of every page it mutates plus `PAGE_INIT` for pages it creates `[PROPOSED]`. A `HEAP_RELAYOUT` record type is reserved, not assigned — the FPI is the honest v1 shape for the same reason chain growth uses one. An unlogged relayout is forbidden even while recovery does not exist: the WAL-before-data gate is store-enforced, and a log that names slots a relayout silently moved is a log that lies. |
| R8 | **The maintenance surface is deliberately empty.** Cabins and secondary indexes are relocation-invariant (value = pk indirection, `cabin.md`, `index.md` B2); the var-heap is untouched (invariant 14); trails are invalidated by the epoch bump and self-heal on next execution. A **heap relation has no pk index**, so a heap-relation mover maintains *nothing but the epoch* — which is why the first mover targets heap relations (§4). `heap-and-tuple.md` §7's "keep the B+ tree consistent" applies only to btree-clustered relations, whose relayout is the tree's own restructure and out of v1's scope entirely; this spec amends that parenthetical. |
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
  literal-coercion path was (`types.md` §3.1).
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
   **The ownership check is built** (2026-08-13): `docs/spec/page.md` §2a puts
   the owning object's oid in the common header's `reserved1` word, so a
   quarantined page whose `owner_oid` resolves to a `kTypeDroppedTable`
   tombstone (or, for index page types, to no `sys.indexes` row) is
   provably orphaned — ABA-proof because neither oid space ever reissues.
   The gate still does not open: reclamation also needs the free map
   (`page.md` §5) and a mover, and every pre-§2a page reads owner 0 —
   permanently unattributed by §2a's no-backfill decision — so quarantine
   remains the rule for those.

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

---

# Part II — Autonomous Advisory Cabin Management (the Cabin controller)

**Merged in at the 2026-08-09 branch merge.** This part was authored as a
standalone "Physical Optimizer v1" spec on a parallel branch, the same day
Part I was written — two sessions, one name, two subjects. The merge decided
one umbrella document: Part I is *relayout* (what `heap-and-tuple.md` §7 has
always meant by the physical optimizer — shadow-only, gated, with the code);
Part II is the **`CABIN AUTO` promotion pipeline** `cabin.md` §8.1 left
open — a per-core background controller over Observational Cabins. The two
are complementary and touch none of each other's structures; Part II
consumes the R1 lazy-decay score Part I defines and implements
(`stats/decay.hpp` — one decay implementation, shared). To keep every
existing citation valid, Part II keeps its own id spaces: decisions `PO1`-
`PO10`, tasks `PHY01`-`PHY08` (workplan Part II), sections `§II.n`. The
component's runtime name stays distinct too — class `CabinOptimizer`, keys
`kds.cabin_optimizer`/`kds.po_*`, view `SHOW CABIN_OPTIMIZER` (PO9 named
it `sys.cabin_optimizer`; PHY06 realized it as a `SHOW` surface for SHOW
ASSERTIONS' reason — the dispatcher holds the controller, the executor
and the collector, and a `sys.*` SELECT path holds none of them) —
because Part I's built `physical_optimizer` config key already means the
shadow report, and one key wearing two meanings is how switches lie.
**Status: ADOPTED (experimental); PHY01 (the S1-S3 signal plumbing and
snapshot, `stats/optimizer_signals.hpp`), PHY02 (the pure decision core,
`stats/cabin_optimizer.hpp` — 16.16 fixed point, the PO5 lifecycle) PHY03 (the decision-log ring, the `kCabinOriginAuto` ownership tag, and
the frozen P_scan baseline — load-bearing, or the controller drops its own
success) and
PHY05 (the §II.6 config surface — the `cabin_optimizer` switch with its
runtime `SET`, and the percent-integer tuning family validated against
the hysteresis gap),
PHY07 (the seed-driven replay harness with checked-in golden traces —
PO10's determinism proven end to end, all 2026-08-09) and
PHY04 (the executor, 2026-08-10 — `exec::CabinOptimizerExecutor`,
ring-routed seeded builds, busy-row deferral, batch heal, PO8 at every
boundary, the expeditor cadence) and
PHY06 (observability, 2026-08-10 — PO9 realized as `SHOW CABIN_OPTIMIZER`
rather than a `sys.*` SELECT, per SHOW ASSERTIONS' rule; applied-action
counters on the executor; ANALYZE's `cabin_optimizer=true` mark on a
managed probe; and the `NoteExtended` completion edge, closing PHY04's
recorded page-accounting gap) and
PHY08 (the E2E close-out, 2026-08-10 — the full lifecycle in one scripted
test observed through the view, and the bench note
`bench/results-cabin-optimizer.md`: zero-candidate overhead unmeasurable
with the tick priced at 2-3 µs CPU, and the improvement case creating
autonomously in exactly 3 ticks and serving at 10.9× on 10,000 rows)
are built. **The Part II series is complete.**

## II.1 Positioning

Cabin optimizer v1 is the first realization of the self-managing-storage
vision: the engine observing its own workload and deciding its own physical
structures. v1 deliberately excludes heap tuple relocation — that path
entangles heap integrity, MVCC, and WAL, and offers no safe failure mode
(it is exactly Part I's gated territory). Instead, v1 operates exclusively
on **Observational (advisory) Cabins**, which have the decisive property
that a wrong decision costs performance only, never correctness: stale
hints heal on read, dangling entries are discarded, and a dropped Cabin
merely returns the system to its baseline.

The component carries the plain technical name — **cabin optimizer**
(class `CabinOptimizer`) — rather than a frontier-lineage codename; it
is engine machinery, not a user-facing storage concept like Keystone,
Waystone, or Cabin.

The cabin optimizer is a per-core background controller that consumes
workload statistics and issues a closed vocabulary of actions over
Observational Cabins. It is experimental in v1, runtime-switchable, and
every decision it makes is logged with the inputs that produced it.

## II.2 Decision Record

| ID | Decision |
|----|----------|
| PO1 | Action vocabulary (closed set): **CREATE** (build a new Cabin for a column combination), **EXTEND** (widen an existing Cabin's value coverage), **HEAL** (batch re-validate location hints), **DROP** (retire a cold or unhealable Cabin). REBUILD is excluded (≡ DROP+CREATE). **Bound Cabins are outside the cabin optimizer's jurisdiction** — owned by assertions, never read, never touched (invariant, debug-asserted). |
| PO2 | Input signals, exactly three: **(S1)** fingerprint execution frequency under the R1 lazy exponential-decay score (Part I R1, implemented in `stats/decay.hpp` — one decay implementation, shared); **(S2)** observed predicate scan cost — pages scanned per execution, from the executor's per-statement counters; **(S3)** Cabin quality — hint hit/failure counters and lookup coverage misses. Buffer-pool miss statistics are deferred to v2 (relation-granular, too coarse for column/value decisions). |
| PO3 | Decision model: **cost–benefit formula** (§II.4). Determinism requirements: the decision core is a **pure function** from a statistics snapshot to an action set, computed in **fixed-point integer arithmetic** (no floats), with hysteresis built in as asymmetric margin factors and cooldowns — a raw cost model oscillates; the margins are load-bearing, not tuning sugar. |
| PO4 | Execution: a background-group task on each relation's **home core**. Independent decisions per core; no cross-core coordination (EV4 spirit). All build/extend scans go through the **scan ring (EV6)** — mandatory, so the cabin optimizer can never displace the foreground working set it is trying to serve. |
| PO5 | Lifecycle state machine per managed Cabin: `CANDIDATE → BUILDING → ACTIVE → DECAYING → DROPPED`, with `DECAYING → ACTIVE` recovery on score rebound and `BUILDING → discard` on failure/interruption. All transitions execute as single home-core steps (atomicity practice established by the AST06 cutover). |
| PO6 | Budget: per-core page budget for optimizer-managed Cabins (`kds.po_page_budget`). Over-budget CREATE is admitted only in **exchange** for dropping the lowest-net-benefit ACTIVE Cabin (explicit replacement rule — optimization within a budget, not open-ended growth). Memory residency is the buffer pool's concern (Observational Cabin pages are evictable, EV3); this budget governs disk and upkeep. |
| PO7 | Refresh strategy: quality surveillance, not eager maintenance. Hint-failure rate above threshold ⇒ HEAL; if quality does not recover after HEAL (e.g., mass relocation by bulk UPDATE) ⇒ DROP — demand, if real, re-nominates the candidate. "Discard and re-observe" over "repair at any cost" is the correct posture for advisory structures. |
| PO8 | Safety: experimental status; runtime kill switch `SET kds.cabin_optimizer = on\|off`. Turning off halts new decisions and in-flight builds but leaves existing Cabins untouched (no destructive path on disable). Every action is recorded in a decision log with the input-score snapshot. |
| PO9 | Observability: a view per managed Cabin — state, net-benefit score, hint hit rate, coverage, pages, last action + reason; production counters per action type and budget utilization; ANALYZE Cabin-hit output gains a flag marking optimizer-managed Cabins. *(Named `sys.cabin_optimizer` when written; realized by PHY06 as `SHOW CABIN_OPTIMIZER` — the naming note above carries the reason.)* |
| PO10 | Deterministic testing: seed-driven statistics streams replayed through the pure decision core reproduce identical action sequences. Structural requirement on the code: **decide (pure, side-effect free) and execute (effectful) are separate phases**. Oracles: no oscillation under stationary workloads, budget invariant, disable-switch harmlessness. |

## II.3 Architecture

```
            (per home core)
  ┌─────────────────────────────────────────┐
  │  Stats collectors (S1,S2,S3) ──► Snapshot│
  │                                     │    │
  │                (pure, fixed-point)  ▼    │
  │            CabinOptimizer::Decide(Snapshot)    │
  │                     │ ActionSet          │
  │                     ▼                    │
  │            CabinOptimizer::Execute ──► Cabin   │
  │             (background task,    machinery│
  │              scan ring, single-  + decision│
  │              step transitions)     log   │
  └─────────────────────────────────────────┘
```

- **Snapshot**: an immutable, versioned aggregation of S1–S3 taken at
  decision time. Snapshot construction is the only stats read; Decide never
  reads live counters (determinism).
- **Decide**: pure function `Snapshot → ActionSet` implementing §II.4. No
  allocation of engine resources, no I/O, no clock reads (the decay epoch
  is part of the snapshot).
- **Execute**: applies actions with the machinery constraints of PO4/PO5.
  Interruptible between cooperative batches; an interrupted BUILDING is
  discarded, never resumed half-built.

## II.4 Cost–benefit model (PO3)

All quantities are in the common currency of **page accesses**, fixed-point
(PROPOSED: 16.16). Per candidate or managed Cabin `c` over the relation's
fingerprint population:

**Benefit** — decayed page savings per unit time:

```
B(c) = Σ_i  f_i × max(0, P_scan,i − P_cabin)
```

- `f_i` — R1-decayed execution frequency of fingerprint `i` whose predicate
  is served by `c` (S1);
- `P_scan,i` — observed pages scanned per execution without the Cabin (S2;
  for an ACTIVE Cabin this is the recorded pre-Cabin baseline carried in
  its state, not a live measurement);
- `P_cabin` — pages touched via Cabin lookup, PROPOSED constant 2
  (directory + target; refined by measurement later).

**Cost** — amortized upkeep per unit time:

```
C(c) = P_rel / T_amort  +  h_fail(c) × f_lookup(c) × k_heal
```

- `P_rel / T_amort` — build cost (full/partial scan of the relation's
  pages) amortized over window `T_amort`, expressed in R1 decay
  half-lives so build cost and benefit decay on the same clock. As
  proposed, `T_amort` = 1 half-life; **ratified at 64, operator-decided
  2026-08-10**, after the business-days scenario
  (`bench/results-cabin-optimizer-days.md`) showed the lifecycle at 1 is
  a *nightly rebuild loop* by this model's own arithmetic — survival is
  log₂(B/C) + 2×T_amort half-lives of silence, and a market overnight at
  the default 600 s half-life is ~105. At 64 the cooldown alone is 128
  half-lives (21 h 20 m at defaults): a Cabin survives any close-to-open
  gap DECAYING and the morning rebound recovers it, while a weekend of
  silence still drops it — deliberate, since re-nomination costs ~3
  ticks and two stale days is what DROP exists for. The window is **one
  belief read by both sides**: raising it lowers the admission bar by
  the same factor (a structure serving for a day need only pay for
  itself over a day), and PO6's budget, not the bar, bounds the
  population. `cabin_optimizer_amort_windows` is the key; 0 is refused
  (a zero window prices every Cabin free). **The confirming rerun
  (`bench/results-cabin-optimizer-days.md` Part II) says 64 is right and
  possibly slightly long**, and its two findings were about `T_cooldown`,
  not about T_amort — see the cooldown's own entry below;
- `h_fail` — hint failure rate (S3), `f_lookup` — decayed lookup
  frequency, `k_heal` — pages per heal event (PROPOSED 2).

**`T_cooldown` is its own parameter (2026-08-10), no longer `2 × T_amort`.**
The rerun's finding was that one number was answering two questions: how
long a build is believed to pay for itself, and how much silence proves
death. Decoupling them is the fix; the default is 128 half-lives —
exactly what the old expression yielded at the shipped window, so nothing
measured moved. `cabin_optimizer_cooldown_half_lives` is the key, in
whole half-lives (integer on purpose: lifting a nanosecond count into
16.16 is what once collapsed a 20-minute cooldown to 4.3 s). **0 is
accepted** where the window's 0 is refused, and the asymmetry is
deliberate — zero time-patience leaves the score hysteresis, which is
coherent; a zero window prices every Cabin free, which is not.

What decoupling does **not** buy is a shorter dwell for a dead Cabin,
and the reason is worth stating because it looks like a tuning failure
and is not: **a dead Cabin and an overnight-quiet one emit the same
signal** — no lookups — so the only thing separating them is waiting
longer than the quiet period. Any cooldown under ~105 half-lives (a
market overnight at the default 600 s) reintroduces the nightly rebuild
loop; below that floor the knob retires *live* Cabins, not dead ones.
The ~21 h a permanently cold Cabin spends in DECAYING is therefore the
price of overnight survival, not a mis-set parameter. The floor is a
property of the workload, so it is documented rather than enforced — a
24/7 workload with no quiet period is exactly the case that may lower
it, and now can.

The second finding — Q24.8 **underflowing to zero after ~16 half-lives**
— is **addressed (2026-08-10)**, and what it turned out to be is worth
recording, because the first reading of it was wrong in a way that would
have produced the wrong fix.

R1 gained a range-preserving read rather than a new representation:
`stats::decay.hpp`'s `Log2ValueAt`. **The information was never lost in
the state** — `{scaled, last_bump}` holds the whole history — only in
the read, where `>> halvings` flushes it. In the log domain decay is a
subtraction, so scores stay ordered for thousands of half-lives; the
linear `ValueAt` is untouched, so every consumer that only ranks live
data is unaffected, and the decision core consults the log **only where
the linear form has run out of resolution**. That is why no measured
threshold decision and no PHY07 golden trace moved.

What it fixes, exactly:
- **The eviction victim among cold entries** (`OptimizerSignals`), which
  is a live defect at every configuration: a full table is mostly idle
  entries, all reading 0, so the "coldest" scan found a tie and kept
  whichever the hash map yielded first. A fingerprint idle an hour could
  outlive one idle a week.
- **The DECAYING onset's cap** at `log2(frequency × 256)` half-lives,
  independent of the saved-pages factor and T_amort. At the shipped
  T_amort = 64 the honest onset lands ~1.3 half-lives *before* the cap,
  so this is a cliff one doubling away rather than a live defect. **The
  crossover is measured at T_amort = 130** (`bench/results-cabin-optimizer-days.md`
  Part III, a controlled A/B against a pre-fix binary), which is nearer
  the shipped window than the 256 first estimated here — the cliff is
  one doubling away, not two. Post-fix the onset tracks `log2(T_amort)`
  as the model says (+6.5 and +11.5 half-lives at windows of 4,096 and
  100,000, against +6.0 and +10.6 predicted); pre-fix it saturated flat
  (+1.0 and +1.5). Removing the cap is what lets the window be widened
  at all.
- **Budget-swap victim ordering** among incumbents whose scores have
  both underflowed.

What it does **not** fix, and cannot: the DROP at the end of the
cooldown is still fired by the clock. A dead Cabin and an
overnight-quiet one emit identical silence at every instant, so no
amount of score precision distinguishes them — the earlier paragraph's
floor is structural, not a resolution problem. "Judgement" here means
the *timing* is proportional to demonstrated value again, not that the
final step consults evidence it does not have.

Precision: the log read is exact in its integer part and LUT-bucketed in
its fraction, worst case 0.78% in the ratio — under a tenth of the
narrowest threshold margin any rule applies.

**Rules** (asymmetric margins = hysteresis; all thresholds PROPOSED,
configuration-surfaced):

| Action | Condition |
|---|---|
| CREATE (CANDIDATE→BUILDING) | `B > θ_create × C` sustained for `N_confirm` consecutive snapshots (θ_create = 3, N_confirm = 3) and budget admits (or replacement rule fires) |
| EXTEND | Cabin ACTIVE and coverage-miss share of lookups > `θ_extend` (= 20%) and the missed share's marginal `B` alone clears `θ_create × ΔC` |
| HEAL | `h_fail > θ_heal` (= 10%) while `B > θ_drop × C` |
| DECAYING (ACTIVE→) | `B < θ_drop × C` (θ_drop = 0.5) |
| DROP (DECAYING→) | condition persists for `T_cooldown` (its own parameter since 2026-08-10, default 128 half-lives; was `2 × T_amort`) — or HEAL already attempted without quality recovery (PO7) |
| recover (DECAYING→ACTIVE) | `B > θ_create × C` again |

The wide gap θ_drop ≪ 1 ≪ θ_create plus `N_confirm`/`T_cooldown` is the
anti-thrash mechanism: a Cabin is created only on strong sustained evidence
and retired only on strong sustained absence of it.

**Budget arbitration (PO6).** If CREATE is justified but the budget is
full: evict the ACTIVE Cabin with the minimum `B − C` iff the candidate's
`B − C` exceeds it by factor `θ_swap` (PROPOSED 2). Otherwise the candidate
waits. This makes the budget a solved ranking problem, not a growth valve.

## II.5 Lifecycle details (PO5)

- **CANDIDATE**: exists only as a decision-log/catalog entry with its
  running evidence; zero pages.
- **BUILDING**: background build through the scan ring; observation-based
  population (this is the Observational class — the builder seeds coverage
  from the predicate values that generated the evidence, plus EXTEND-style
  widening if the decision so specifies). Interruption or kill-switch ⇒
  discard, log, return to CANDIDATE.
- **ACTIVE**: normal advisory service; quality counters accumulate.
- **DECAYING**: no EXTEND/HEAL spending; pages remain (buffer pool evicts
  cold ones naturally per EV3); recoverable.
- **DROPPED**: pages reclaimed, catalog state removed, decision logged.
  Re-nomination starts from scratch as CANDIDATE.

Jurisdiction invariant: the cabin optimizer enumerates only Observational
Cabins it created (ownership tag in the Cabin catalog state). Bound Cabins
and any future manually-declared Cabins are invisible to it.

## II.6 Configuration surface

| Setting | Default (PROPOSED) | Notes |
|---|---|---|
| `kds.cabin_optimizer` | off (experimental) | runtime switch, non-destructive off |
| `kds.po_page_budget` | pool/8 (disk pages, per core) | PO6 |
| `kds.po_theta_create` / `_drop` / `_swap` / `_extend` / `_heal` | 3 / 0.5 / 2 / 0.2 / 0.1 | fixed-point |
| `kds.po_confirm_snapshots` | 3 | N_confirm |
| `kds.po_amort_window` | ~~R1 half-life~~ **64 half-lives (ratified 2026-08-10)** | T_amort — the build-cost amortization window; see §II.4's cost note. Built as `cabin_optimizer_amort_windows`. |
| `kds.po_cooldown` | **128 half-lives** | T_cooldown — the DECAYING dwell, decoupled from T_amort 2026-08-10 (it was `2 ×` it). Built as `cabin_optimizer_cooldown_half_lives`; 0 accepted. Cannot usefully sit below a workload's longest quiet period — §II.4. |
| `kds.po_snapshot_interval` | 10 s | decision cadence |

## II.7 Non-goals (v1)

- Heap tuple relocation / clustering — explicitly out (the headline scope
  decision of Part II; it is Part I's gated territory).
- Bound Cabin management — assertion-owned, permanently out of the cabin
  optimizer's jurisdiction.
- Buffer-pool-miss-driven signals, cross-core coordination, learned
  decision models — deferred (v2 candidates; learned models additionally
  conflict with the determinism contract and need a separate decision).
- Index (B+tree) creation/dropping — the cabin optimizer's vocabulary is
  Cabins only in v1; auto-indexing is a far larger contract surface.
- User-facing hints to steer the cabin optimizer (`PIN`, `FORBID`) —
  reserved grammar space, not in v1.
