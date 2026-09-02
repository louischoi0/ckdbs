# Physical Optimizer — Two Parts: Relayout (Part I) and the Cabin Controller (Part II)

**One umbrella, two halves.** Part I (this file through §10) is
**statistics-driven relayout** — shadow-only, decisions `R1`-`R12`: the
decay score, the page epoch, the planner and `SHOW RELAYOUT` exist; no
mover does. Part II (from the divider after §10) is **autonomous advisory
Cabin management** — the `CABIN AUTO` promotion pipeline, decisions
`PO1`-`PO10`, built. Part II consumes Part I's R1 decay score and neither
part touches the other's structures.

Markers: `[CONFIRMED]` is settled; `[PROPOSED]` marks a default that
shipped as written and is amendable by measurement; `[OPEN]` must not be
assumed.
Related documents: `heap-and-tuple.md` §7 (the normative relayout section
this file expands), `rule-fixed-length-tuple.md` (tuple mobility),
`eviction.md` (EV1's temperature hook), `pattern-tracking-levels.md`
(decay-ranked trail eviction), `waystone-concpets.md` §3.1 (epoch validation),
`cabin.md` (relocation invariance), `index.md` (IX3),
`txn.md` §9 (no reader registration), `parser-v2.md` I7/V11.

**On the numbering.** `eviction.md` EV1 and `pattern-tracking-levels.md`
§3 cite "the physical-optimizer lazy-decay score (R1)", and **R1 is the
lazy-decay score** here so those citations resolve. `parser-v2.md` I15
also names an "R1" (the no-fetch-under-span rule): cite the file with the
number.

---

## 1. Positioning

Engine-driven physical optimization is one of the two things this project
exists for (`heap-and-tuple.md` §1). Its input half is built —
`sys.access_stats`, the `kFilterScan` split, the index-kind split — and
this spec is what consumes it.

**Relayout is shadow-only, and that is the finding, not a hedge.** Every
candidate move is blocked by a named gate (§6): compaction needs the
reader horizon that deliberate non-registration withholds, hot clustering
breaks the ordered-between property `kRange` pruning reads, and retiring a
page for reuse breaks trail validation across relations. So the optimizer
applies its own promotion-gate philosophy (`overview.md` §4) to itself:
**observe, classify, plan, report — enact nothing.**

What ships:

1. **The lazy-decay score (R1)** — the one time-decay implementation
   three subsystems share.
2. **The page epoch (R4)** — the field, its discipline, and real reads at
   Waystone's and the Cabin's validation sites.
3. **The planner and `SHOW RELAYOUT` (R9, R10)** — the physical health
   report: what would be done, what it would buy, and which gate blocks it.

No mover exists; nothing moves a tuple, and nothing bumps a page epoch.

---

## 2. Decision Record

| ID | Decision |
|----|----------|
| R1 | **The lazy-decay score.** Exponential half-life decay computed lazily from a stored `{score, last_bump}` pair: a touch decays-then-increments, a read decays only, and there is no background decay pass — idle data costs nothing and is never visited. One implementation (`include/kds/stats/decay.hpp`), `sched::Clock`-injected; with no clock the score degrades to a raw count, the same best-effort stance `sys.access_stats.last_seen` already takes. Half-life is the `decay_half_life` config key, per instance, default 600 s `[PROPOSED]`. Declared consumers: hot/cold classification here, trail-retention ordering (`pattern-tracking-levels.md`), and EV1's experimental temperature hook. Scores are memory-resident and never persisted `[PROPOSED]`. **Two reads:** the Q24.8 `ValueAt` — which underflows to zero after ~16 half-lives, so it ranks live data only — and `Log2ValueAt`, the same state read in the log domain, where decay is a subtraction and ordering survives indefinitely. Any consumer that must order *idle* things (an eviction victim, a retirement) uses the log read (§II.4). |
| R2 | **Inputs are the existing collectors only**: `sys.access_stats` (the shape axis), Waystone sightings (the value axis), and what a page itself says when walked. The optimizer adds no third collector; an input it lacks becomes a collection change in the layer that owns collection, spec'd there first. |
| R3 | **Two halves with a hard seam.** The **planner** is pure — it reads statistics and the catalog and produces `RelayoutPlan`s with predicted benefit — and the **mover** enacts plans. Shadow mode is the planner without the mover. The `physical_optimizer` config key takes `off | shadow` (default `shadow`); `on` is **refused at startup naming the gates**, so a config written for a future the engine does not have fails loudly instead of silently under-delivering. |
| R4 | **The page epoch**: `PageHeaderFields::reserved0` (offset 16, u64) is `relayout_epoch`. Every existing page carries 0 there, so **no format bump**: a zero reads as epoch 0. Durable by construction (it is header bytes), which trails need because trail pages are durable. Bumped **only by a mover** when tuples move; INSERT/UPDATE/DELETE never bump, because the fixed-length rule makes them address-stable — that stability is the whole reason replay is safe. Wraparound is unreachable at u64 width rather than handled. **Pairing rule: no consumer may accept a location on epoch equality alone** — the epoch is a fast whole-page invalidation layered over the Keystone-id check (K1), never a substitute for it. |
| R5 | **The legal-move table (§4)** is normative for any mover. It derives from invariants 2, 3, 4, 8, 14, from `kRange` pruning's ordered-between dependency, and from rollback's in-memory undo trail naming addresses. |
| R6 | **Mover execution context**: a maintenance-group task on the relation's home core, never cross-core, run-to-completion with no in-flight statement holding a position on the relation and no open transaction whose undo trail names addresses in it. |
| R7 | **Mover logging**: a full-page image of every page it mutates plus `PAGE_INIT` for pages it creates `[PROPOSED]`. A `HEAP_RELAYOUT` record type is reserved, not assigned. An unlogged relayout is forbidden: the WAL-before-data gate is store-enforced, and a log that names slots a relayout silently moved is a log that lies. |
| R8 | **The maintenance surface is deliberately empty.** Cabins and secondary indexes are relocation-invariant (value = pk indirection, `cabin.md`, `index.md` B2); the var-heap is untouched (invariant 14); trails are invalidated by the epoch bump and self-heal on next execution. A **heap relation has no pk index**, so a heap-relation mover maintains *nothing but the epoch*. `heap-and-tuple.md` §7's "keep the B+ tree consistent" applies only to btree-clustered relations, whose relayout is the tree's own restructure and outside this spec; this spec amends that parenthetical. |
| R9 | **The benefit model**: predicted benefit = pages-not-touched per execution × decayed shape frequency, reported per plan in pages and per shape. The planner's output carries a measured-after field beside the prediction so the two are comparable in one format. |
| R10 | **The planner is pull-only**: computed when `SHOW RELAYOUT` asks, no background task, no cadence. Zero idle cost, no timing-wheel dependency. |
| R11 | **Scope**: R1 + R4 + the planner and report. No mover. Every enactment is blocked by a named gate (§6) and the report says which. |
| R12 | **Per-relation gate**: none exists — `parser-v2.md` V11's `WITH (PHYSICAL_OPTIMIZER = ON\|OFF)` flag is unbuilt. The planner reports every relation regardless: a report is free of risk, and an operator who opted a relation out would still want to see what that declines. |

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
  literal-coercion path (`types.md` §3.1).
- **No clock, no decay**: the score degrades to a raw counter. Deterministic
  tests inject `ManualClock` and get exact halving.
- Fixed-point arithmetic `[PROPOSED]`: u32 score scaled by 256, shift/mask
  only, no floating point on any statement path (`rules.md`).
- The half-life is one instance-wide config key. Per-consumer half-lives do
  not exist; one would be a new key, not a parameter that silently forks the
  meaning of "hot".

---

## 4. The legal-move table (R5) — normative for any mover

Any relayout of a heap relation operates under exactly these rules.

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
- Target a btree-clustered relation `[PROPOSED]`: a btree leaf is a heap
  page, so "relayout" there is a tree restructure with descent consistency
  to preserve — a different feature.
- Return a retired page to the free map (§6, gate 3). Retired pages are
  **quarantined** — held out of every allocator. A quarantine leaks; the
  leak is the honest price and is bounded by how much relayout runs.

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
  its verdict: `blocked_on=<gate>`, always.

Three plan kinds, none enactable:

| Plan kind | What it would do | Blocked on |
|---|---|---|
| `compact` | Rewrite the chain dropping delete-marked tuples past the reader horizon; reclaim emptied pages | Gate 1 |
| `cluster` | Co-locate a hot set on fewer pages | Gate 2 |
| `defrag` | Rewrite a chain onto contiguous page ids for sequential I/O | Gate 3 |

The report is the deliverable: it turns "should a gate open" from taste
into a number, per relation, on a live workload.

---

## 6. The gates — why nothing is enacted

Every relayout move is blocked, and `SHOW RELAYOUT` names the gate. The
three gates, as facts about today's engine:

1. **Reader horizon.** Readers are unregistered (`txn.md` §9), so no
   delete-marked tuple can be proven unneeded by every snapshot; nothing
   compacts.
2. **Ordered-between compatibility.** Clustering an arbitrary hot id set
   onto one page can satisfy invariant 3 while breaking the between-pages
   ordering `kRange` pruning reads; nothing clusters.
3. **Cross-relation page reuse.** Trail validation checks `rel_oid` and the
   Keystone id at the recorded `(page_id, slot)`, ids are issued per
   relation, and `PAGE_INIT` writes epoch 0 — so a page freed from relation
   A and reallocated to B could validate a stale trail entry. `page.md`
   §2a's `owner_oid` makes a retired page provably orphaned when its oid
   resolves to a tombstone, but a page written before §2a reads owner 0
   permanently, so retired pages are quarantined (§4), never reused.

The decisions that would open a gate are not recorded here.

---

## 7. The epoch lands here (R4)

- `relayout_epoch` is read and written through `page_header.hpp` accessors;
  `PAGE_INIT` and every page-format path leave it 0. No format bump: every
  existing page already reads 0.
- **Waystone**: the recorder stores the page's current epoch in the trail
  entry; replay compares recorded against current, and a mismatch is a
  per-entry miss with the ordinary fall-through (`waystone-concpets.md`
  §3.1 rule 2).
- **Cabin**: `CabinEntry::page_epoch` is recorded from the header and
  compared in `exec/tuple_verify.hpp` — the one shared verifier, so Waystone
  and Cabin get the real check at one site.
- With no mover every comparison is between two zeros — **the check is real
  and its inputs are constant**, which is the state the contract tests pin:
  a test that hand-bumps a page's epoch must see replay miss, heal, and
  answer byte-identically.

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

Not in this engine: a mover, btree-clustered relation relayout,
temperature-unified eviction, score persistence, per-consumer half-lives,
and per-pattern hot-set clustering. The decisions are not recorded here.

---

# Part II — Autonomous Advisory Cabin Management (the Cabin controller)

Part II is the **`CABIN AUTO` promotion pipeline** `cabin.md` §8.1 names —
a per-core background controller over Observational Cabins. It consumes
the R1 lazy-decay score Part I defines (`stats/decay.hpp` — one decay
implementation, shared) and touches none of Part I's structures. It keeps
its own id spaces: decisions `PO1`-`PO10`, sections `§II.n`. The
component's runtime name is distinct too — class `CabinOptimizer`, config
keys `cabin_optimizer*` (§II.6), view `SHOW CABIN_OPTIMIZER` (a `SHOW`
rather than a `sys.*` relation, for `SHOW ASSERTIONS`' reason: the
dispatcher holds the controller, the executor and the collector, and a
`sys.*` SELECT path holds none of them) — because Part I's
`physical_optimizer` key already means the shadow report, and one key
wearing two meanings is how switches lie.

**Status: built (experimental).** The signal plumbing and snapshot
(`stats/optimizer_signals.hpp`), the pure decision core
(`stats/cabin_optimizer.hpp` — 16.16 fixed point, the PO5 lifecycle), the
decision-log ring, the `kCabinOriginAuto` ownership tag, the frozen
P_scan baseline (load-bearing, or the controller drops its own success),
the §II.6 config surface, the executor (`exec::CabinOptimizerExecutor`:
ring-routed seeded builds, busy-row deferral, batch heal, PO8 at every
boundary, the expeditor cadence), the observability surface (`SHOW
CABIN_OPTIMIZER`, applied-action counters, ANALYZE's
`cabin_optimizer=true` mark on a managed probe), and the seed-driven
replay harness with checked-in golden traces (PO10's determinism, end to
end).

## II.1 Positioning

The cabin optimizer is the first realization of the self-managing-storage
vision: the engine observing its own workload and deciding its own physical
structures. It excludes heap tuple relocation — that path entangles heap
integrity, MVCC, and WAL, and offers no safe failure mode (it is exactly
Part I's gated territory). It operates exclusively on **Observational
(advisory) Cabins**, which have the decisive property that a wrong decision
costs performance only, never correctness: stale hints heal on read,
dangling entries are discarded, and a dropped Cabin merely returns the
system to its baseline.

The component carries the plain technical name — **cabin optimizer**
(class `CabinOptimizer`) — rather than a frontier-lineage codename; it
is engine machinery, not a user-facing storage concept like Keystone,
Waystone, or Cabin.

The cabin optimizer is a per-core background controller that consumes
workload statistics and issues a closed vocabulary of actions over
Observational Cabins. It is experimental, runtime-switchable, and every
decision it makes is logged with the inputs that produced it.

## II.2 Decision Record

| ID | Decision |
|----|----------|
| PO1 | Action vocabulary (closed set): **CREATE** (build a new Cabin for a column combination), **EXTEND** (widen an existing Cabin's value coverage), **HEAL** (batch re-validate location hints), **DROP** (retire a cold or unhealable Cabin). REBUILD is excluded (≡ DROP+CREATE). **Bound Cabins are outside the cabin optimizer's jurisdiction** — owned by assertions, never read, never touched (invariant, debug-asserted). |
| PO2 | Input signals, exactly three: **(S1)** fingerprint execution frequency under the R1 lazy exponential-decay score (`stats/decay.hpp` — one decay implementation, shared); **(S2)** observed predicate scan cost — pages scanned per execution, from the executor's per-statement counters; **(S3)** Cabin quality — hint hit/failure counters and lookup coverage misses. Buffer-pool miss statistics are not an input (relation-granular, too coarse for column/value decisions). |
| PO3 | Decision model: **cost–benefit formula** (§II.4). Determinism requirements: the decision core is a **pure function** from a statistics snapshot to an action set, computed in **fixed-point integer arithmetic** (no floats), with hysteresis built in as asymmetric margin factors and cooldowns — a raw cost model oscillates; the margins are load-bearing, not tuning sugar. |
| PO4 | Execution: a background-group task on each relation's **home core**. Independent decisions per core; no cross-core coordination (EV4 spirit). All build/extend scans go through the **scan ring (EV6)** — mandatory, so the cabin optimizer can never displace the foreground working set it is trying to serve. |
| PO5 | Lifecycle state machine per managed Cabin: `CANDIDATE → BUILDING → ACTIVE → DECAYING → DROPPED`, with `DECAYING → ACTIVE` recovery on score rebound and `BUILDING → discard` on failure/interruption. All transitions execute as single home-core steps. |
| PO6 | Budget: per-core page budget for optimizer-managed Cabins (`cabin_optimizer_page_budget`). Over-budget CREATE is admitted only in **exchange** for dropping the lowest-net-benefit ACTIVE Cabin (explicit replacement rule — optimization within a budget, not open-ended growth). Memory residency is the buffer pool's concern (Observational Cabin pages are evictable, EV3); this budget governs disk and upkeep. |
| PO7 | Refresh strategy: quality surveillance, not eager maintenance. Hint-failure rate above threshold ⇒ HEAL; if quality does not recover after HEAL (e.g., mass relocation by bulk UPDATE) ⇒ DROP — demand, if real, re-nominates the candidate. "Discard and re-observe" over "repair at any cost" is the correct posture for advisory structures. |
| PO8 | Safety: experimental status; runtime kill switch `SET cabin_optimizer = on\|off`. Turning off halts new decisions and in-flight builds but leaves existing Cabins untouched (no destructive path on disable). Every action is recorded in a decision log with the input-score snapshot. |
| PO9 | Observability: `SHOW CABIN_OPTIMIZER` — per managed Cabin: state, net-benefit score, hint hit rate, coverage, pages, last action + reason; production counters per action type and budget utilization; ANALYZE's Cabin-hit output carries `cabin_optimizer=true` on an optimizer-managed Cabin. |
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
16.16. Per candidate or managed Cabin `c` over the relation's fingerprint
population:

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
  (directory + target).

**Cost** — amortized upkeep per unit time:

```
C(c) = P_rel / T_amort  +  h_fail(c) × f_lookup(c) × k_heal
```

- `P_rel / T_amort` — build cost (full/partial scan of the relation's
  pages) amortized over window `T_amort`, expressed in R1 decay
  half-lives so build cost and benefit decay on the same clock.
  **`T_amort` is 64 half-lives** (`cabin_optimizer_amort_windows`; 0 is
  refused, since a zero window prices every Cabin free). The value was
  chosen from a business-days scenario measured against the `bench/` tree
  at `1769487`: survival is log₂(B/C) + 2×T_amort half-lives of silence,
  a market overnight at the default 600 s half-life is ~105 half-lives,
  and at a window of 1 the lifecycle is a nightly rebuild loop by the
  model's own arithmetic. The window is **one belief read by both
  sides**: raising it lowers the admission bar by the same factor (a
  structure serving for a day need only pay for itself over a day), and
  PO6's budget, not the bar, bounds the population.
- `h_fail` — hint failure rate (S3), `f_lookup` — decayed lookup
  frequency, `k_heal` — pages per heal event (PROPOSED 2).

**`T_cooldown` is its own parameter**, not a multiple of `T_amort`: the
two answer different questions — how long a build is believed to pay for
itself, and how much silence proves death. `cabin_optimizer_cooldown_half_lives`
is the key, in whole half-lives (integer on purpose: a nanosecond count
lifted into 16.16 collapses), default 128. **0 is accepted** where the
window's 0 is refused, and the asymmetry is deliberate — zero
time-patience leaves the score hysteresis, which is coherent; a zero
window prices every Cabin free, which is not.

**The cooldown has a structural floor, documented rather than enforced.**
A dead Cabin and an overnight-quiet one emit the same signal — no
lookups — so the only thing separating them is waiting longer than the
quiet period, and the DROP at the end of the cooldown is fired by the
clock, not by evidence the controller does not have. Any cooldown shorter
than the workload's longest quiet period (~105 half-lives for a market
overnight at defaults) retires *live* Cabins, not dead ones. The ~21 h a
permanently cold Cabin spends in DECAYING at defaults is the price of
overnight survival, not a mis-set parameter; a 24/7 workload with no
quiet period may lower it.

**Where the log read is used.** The linear `ValueAt` underflows to zero
after ~16 half-lives, and the decision core consults `Log2ValueAt` (R1)
only where the linear form has run out of resolution: the eviction victim
among cold entries in `OptimizerSignals` (a full table is mostly idle
entries, and a tie there would keep whichever the hash map yielded
first); the DECAYING onset, which the linear read capped at
`log2(frequency × 256)` half-lives independent of `T_amort` and which
now tracks `log2(T_amort)` as the model says; and budget-swap victim
ordering among incumbents whose linear scores have both underflowed. No
threshold decision moves: `ValueAt` is untouched for every consumer that
ranks live data. Precision: the log read is exact in its integer part and
LUT-bucketed in its fraction, worst case 0.78% in the ratio — under a
tenth of the narrowest threshold margin any rule applies.

**Rules** (asymmetric margins = hysteresis; all thresholds PROPOSED,
configuration-surfaced):

| Action | Condition |
|---|---|
| CREATE (CANDIDATE→BUILDING) | `B > θ_create × C` sustained for `N_confirm` consecutive snapshots (θ_create = 3, N_confirm = 3) and budget admits (or replacement rule fires) |
| EXTEND | Cabin ACTIVE and coverage-miss share of lookups > `θ_extend` (= 20%) and the missed share's marginal `B` alone clears `θ_create × ΔC` |
| HEAL | `h_fail > θ_heal` (= 10%) while `B > θ_drop × C` |
| DECAYING (ACTIVE→) | `B < θ_drop × C` (θ_drop = 0.5) |
| DROP (DECAYING→) | condition persists for `T_cooldown` (default 128 half-lives) — or HEAL already attempted without quality recovery (PO7) |
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
Cabins it created (`kCabinOriginAuto` in the Cabin catalog state). Bound
Cabins and manually-declared Cabins are invisible to it.

## II.6 Configuration surface

| Setting | Default | Notes |
|---|---|---|
| `cabin_optimizer` | off (experimental) | runtime switch (`SET`), non-destructive off (PO8) |
| `cabin_optimizer_page_budget` | 1024 pages, per core | PO6 |
| `cabin_optimizer_theta_create_pct` / `_drop_pct` / `_swap_pct` / `_extend_pct` / `_heal_pct` | 300 / 50 / 200 / 20 / 10 | percent integers; validated against the hysteresis gap |
| `cabin_optimizer_confirm_snapshots` | 3 | N_confirm |
| `cabin_optimizer_amort_windows` | 64 half-lives | T_amort — the build-cost amortization window (§II.4); 0 refused |
| `cabin_optimizer_cooldown_half_lives` | 128 half-lives | T_cooldown — the DECAYING dwell (§II.4); 0 accepted; cannot usefully sit below a workload's longest quiet period |
| `cabin_optimizer_snapshot_interval_ms` | 10,000 | decision cadence |

## II.7 Non-goals

The cabin optimizer does not relocate or cluster heap tuples (Part I's
gated territory); does not manage Bound Cabins (assertion-owned,
permanently outside its jurisdiction); takes no buffer-pool-miss signal,
coordinates across no cores, and uses no learned decision model (a learned
model conflicts with the determinism contract); creates and drops no
index — its vocabulary is Cabins only; and accepts no user-facing hint
(`PIN`, `FORBID`).
