# Physical Optimizer — the Shared Foundations (Part I) and the Cabin Controller (Part II)

**One umbrella, two halves.** Part I (this file through §10) is what the
engine's physical optimization rests on: the lazy-decay score (R1) and the
page epoch (R4). No relayout planner or mover exists, and a config file
setting `physical_optimizer` is refused at startup by name (§8). Part II (from the divider after §10) is **autonomous
advisory Cabin management** — the `CABIN AUTO` promotion pipeline,
decisions `PO1`-`PO10`, built — and consumes Part I's R1.

Markers: `[CONFIRMED]` is settled; `[PROPOSED]` marks a default that
shipped as written and is amendable by measurement; `[OPEN]` must not be
assumed.
Related documents: `heap-and-tuple.md` §3.1a and §7, `eviction.md` (EV1's
temperature hook), `pattern-tracking-levels.md` (decay-ranked trail
eviction), `waystone-concpets.md` §2 rule 2 (epoch validation), `cabin.md`
(relocation invariance), `drop-table.md` DT1 (gate 3).

**On the numbering.** `eviction.md` EV1 and `pattern-tracking-levels.md`
cite "the physical-optimizer lazy-decay score (R1)", and **R1 is the
lazy-decay score** here so those citations resolve; R4 and §3, §6 and §7
keep their numbers for the same reason. `parser-v2.md` I15 also names an
"R1" (the no-fetch-under-span rule): cite the file with the number.

---

## 1. Positioning

Engine-driven physical optimization is one of the two things this project
exists for (`heap-and-tuple.md` §1). Its input half is built —
`sys.access_stats`, the `kFilterScan` split, the index-kind split
(`heap-and-tuple.md` §7) — and its acting half is the Cabin controller
(Part II). Nothing moves a tuple for statistics' sake (§6).

---

## 2. Decision Record

| ID | Decision |
|----|----------|
| R1 | **The lazy-decay score.** Exponential half-life decay computed lazily from a stored `{score, last_bump}` pair: a touch decays-then-increments, a read decays only, and there is no background decay pass — idle data costs nothing and is never visited. One implementation (`include/kds/stats/decay.hpp`), `sched::Clock`-injected; with no clock the score degrades to a raw count, the same best-effort stance `sys.access_stats.last_seen` already takes. Half-life is the `decay_half_life` config key, per instance, default 600 s `[PROPOSED]`. Declared consumers: the Cabin controller (Part II), trail-retention ordering (`pattern-tracking-levels.md`), and EV1's experimental temperature hook. Scores are memory-resident and never persisted `[PROPOSED]`. **Two reads:** the Q24.8 `ValueAt` — which underflows to zero after ~16 half-lives, so it ranks live data only — and `Log2ValueAt`, the same state read in the log domain, where decay is a subtraction and ordering survives indefinitely. Any consumer that must order *idle* things (an eviction victim, a retirement) uses the log read (§II.4). |
| R4 | **The page epoch**: `PageHeaderFields::reserved0` (offset 16, u64) is `relayout_epoch`. Every existing page carries 0 there, so **no format bump**: a zero reads as epoch 0. Durable by construction (it is header bytes), which trails need because trail pages are durable. Bumped only when tuples move - today by a btree leaf division and an in-place leaf compaction (`heap-and-tuple.md` §4.1); INSERT/UPDATE/DELETE never bump, because the fixed-length rule makes them address-stable — that stability is the whole reason replay is safe. Wraparound is unreachable at u64 width rather than handled. **Pairing rule: no consumer may accept a location on epoch equality alone** — the epoch is a fast whole-page invalidation layered over the Keystone-id check (K1), never a substitute for it. |
| R2 | **Inputs are the existing collectors only**: the Cabin controller (Part II) reads `sys.access_stats` (the shape axis) and the Cabin store's counters through `stats/optimizer_signals.hpp`. An input it lacks becomes a collection change in the layer that owns collection, spec'd there first. |

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

## 6. Why nothing moves a tuple

No relayout mover exists, and every user relation is a clustered B+ tree
(`heap-and-tuple.md` §5), so a move would be a tree restructure. Three
facts about today's engine stand in front of one:

1. **Reader horizon.** `ReadHorizon()` bounds a purge (`txn.md` §4.1); a
   delete-marked user row is retired only by `PURGE`
   (`heap-and-tuple.md` §4.1c), and nothing compacts a relation for space.
2. **Ordered-between compatibility.** Clustering an arbitrary hot id set
   onto one page would break the between-pages key order that the descent
   and `kRange` pruning read.
3. **Cross-relation page reuse.** Trail validation checks `rel_oid` and the
   Keystone id at the recorded `(page_id, slot)`, ids are issued per
   relation, and `PAGE_INIT` writes epoch 0 — so a page freed from relation
   A and reallocated to B could validate a stale trail entry. **Answered
   for a dropped relation's pages** (`drop-table.md` DT1,
   `instructions/v3.0.0/workorder-bf-drop-table-page-reclaim.md` BF-Q17
   (a)):
   - the reclaim frees only pages its owner walk checked;
   - it frees nothing before no replay can name a page;
   - the one verifier checks the page's class and `owner_oid` against the
     location's relation, so a reused page is a miss (BF-R10).

   Every mountable volume's user pages carry `owner_oid`, so no mountable
   volume holds a page that reads owner 0.

---

## 7. The epoch lands here (R4)

- `relayout_epoch` is read and written through `page_header.hpp` accessors;
  `PAGE_INIT` and every page-format path leave it 0. No format bump: every
  existing page already reads 0.
- **Waystone**: the recorder stores the page's current epoch in the trail
  entry; replay compares recorded against current, and a mismatch is a
  per-entry miss with the ordinary fall-through (`waystone-concpets.md`
  §2 rule 2).
- **Cabin**: `CabinEntry::page_epoch` is recorded from the header and
  compared in `exec/tuple_verify.hpp` — the one shared verifier, so Waystone
  and Cabin get the real check at one site.
- No relayout mover exists, so a page's epoch moves only when a btree leaf
  divides or is compacted in place (`heap-and-tuple.md` §4.1, §4.1c). The
  contract tests pin the check: a test that hand-bumps a page's epoch must
  see replay miss, heal, and answer byte-identically.

---

## 8. Config and surface

| Key | Values | Default | Meaning |
|---|---|---|---|
| `decay_half_life` | seconds, > 0 | 600 `[PROPOSED]` | R1's half-life, instance-wide |
| `physical_optimizer` | — | — | Retired since BZ: a file naming it is refused at startup by name (`Expeditor::Config::ApplyFile`'s retired keys) |

Nothing is reserved: `relayout` is an ordinary identifier, and
`SHOW RELAYOUT` is refused as an unknown `SHOW` target.

---

## 9. Required tests

- Decay unit tests under `ManualClock`: exact halving, touch-vs-read,
  no-clock degradation to a raw count.
- Epoch round-trip: field read/write and `PAGE_INIT` zeroing.
- The hand-bumped-epoch contract test, in both suites: Waystone replay and a
  Cabin resolve against a page whose epoch was bumped by the test must fall
  through per entry and answer byte-identically to the authoritative path.

---

## 10. Out of scope

Not in this engine: a relayout planner or mover, temperature-unified
eviction, score persistence, per-consumer half-lives, and per-pattern
hot-set clustering. The decisions are not recorded here.

**Affinity is not an input here** (AR0-5 D18). D18 keeps a relation's
affinity as a statistic with a weight-0 consumer (AR0 D10) until AS-E.
Nothing stores it (`sys.tables.owner_core` is a reserved word, written 0,
never read), and the declaration a consumer would read is the namespace a
relation was created in (`namespace.md` NS10), already on its `sys.tables`
row.

---

# Part II — Autonomous Advisory Cabin Management (the Cabin controller)

Part II is the **`CABIN AUTO` promotion pipeline** `cabin.md` §8.1 names —
one background controller for the instance over Observational Cabins. It consumes
the R1 lazy-decay score Part I defines (`stats/decay.hpp` — one decay
implementation, shared) and touches none of Part I's structures. It keeps
its own id spaces: decisions `PO1`-`PO10`, sections `§II.n`. The
component's runtime name is distinct too — class `CabinOptimizer`, config
keys `cabin_optimizer*` (§II.6), view `SHOW CABIN_OPTIMIZER` (a `SHOW`
rather than a `sys.*` relation, for `SHOW ASSERTIONS`' reason: the
dispatcher holds the controller, the executor and the collector, and a
`sys.*` SELECT path holds none of them).

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
structures. It excludes tuple relocation — that path entangles page
integrity, MVCC, and WAL, and offers no safe failure mode (§6). It operates exclusively on **Observational
(advisory) Cabins**, which have the decisive property that a wrong decision
costs performance only, never correctness: stale hints heal on read,
dangling entries are discarded, and a dropped Cabin merely returns the
system to its baseline.

It is one background controller for the instance, issuing a closed
vocabulary of actions over Observational Cabins; experimental,
runtime-switchable, and every decision is logged with the inputs that
produced it.

## II.2 Decision Record

| ID | Decision |
|----|----------|
| PO1 | Action vocabulary (closed set): **CREATE** (build a new Cabin for a column combination), **EXTEND** (widen an existing Cabin's value coverage), **HEAL** (batch re-validate location hints), **DROP** (retire a cold or unhealable Cabin). REBUILD is excluded (≡ DROP+CREATE). **Bound Cabins are outside the cabin optimizer's jurisdiction** — owned by assertions, never read, never touched (invariant, debug-asserted). |
| PO2 | Input signals, exactly three: **(S1)** fingerprint execution frequency under the R1 lazy exponential-decay score (`stats/decay.hpp` — one decay implementation, shared); **(S2)** observed predicate scan cost — pages scanned per execution, from the executor's per-statement counters; **(S3)** Cabin quality — hint hit/failure counters and lookup coverage misses. Buffer-pool miss statistics are not an input (relation-granular, too coarse for column/value decisions). |
| PO3 | Decision model: **cost–benefit formula** (§II.4). Determinism requirements: the decision core is a **pure function** from a statistics snapshot to an action set, computed in **fixed-point integer arithmetic** (no floats), with hysteresis built in as asymmetric margin factors and cooldowns — a raw cost model oscillates; the margins are load-bearing, not tuning sugar. |
| PO4 | Execution: **one controller for the instance**, ticked by a background-group task on core 0's cadence (AT-S8 step 3: every core's dispatcher is handed the one `OptimizerSurface` - the switch, the collector every core feeds, and the controller and executor behind a view latch). **A build announces each seed's set before its scan and asks the banking gate** (AY-S6; `cabin.md` §6, §6a), as the serve path's build does, so a write another core lands behind the walk is in the set it banks - which the foreign key's clearing return relies on (`foreign-keys.md` §3a). Until AY-S6 it walked and then committed, and such a write was lost. All build/extend scans go through the **scan ring (EV6)** — mandatory, so the cabin optimizer can never displace the foreground working set it is trying to serve. |
| PO5 | Lifecycle state machine per managed Cabin: `CANDIDATE → BUILDING → ACTIVE → DECAYING → DROPPED`, with `DECAYING → ACTIVE` recovery on score rebound and `BUILDING → discard` on failure/interruption. All transitions execute as single steps of the controller's tick. |
| PO6 | Budget: one page budget for the instance's optimizer-managed Cabins (`cabin_optimizer_page_budget`). Over-budget CREATE is admitted only in **exchange** for dropping the lowest-net-benefit ACTIVE Cabin (explicit replacement rule — optimization within a budget, not open-ended growth). Observational entry sets are memory-resident (`cabin.md` §9), so a Cabin's "pages" are a pricing proxy, 1 + entries / 254 (`CabinOptimizerExecutor::PagesProxyOf`); the budget governs that size and upkeep. |
| PO7b | **The candidate a fingerprint names is read in written-position order since CC-Q15** (`instructions/v3.2.0/workorder-cc-plan-statistics.md`): the first column Cabin probe, else the first equality filter scan, by step id - which since CB-Q3 is the relation's written position - so the join planner's executed order does not choose among them. The planner's order can still change a step's kind (a relation probed in one order is filter-scanned in another), and with it the candidate; the Cabin controller's other inputs are unchanged. |
| PO7 | Refresh strategy: quality surveillance, not eager maintenance. Hint-failure rate above threshold ⇒ HEAL; if quality does not recover after HEAL (e.g., mass relocation by bulk UPDATE) ⇒ DROP — demand, if real, re-nominates the candidate. "Discard and re-observe" over "repair at any cost" is the correct posture for advisory structures. |
| PO8 | Safety: experimental status; runtime kill switch `SET cabin_optimizer = on\|off`. Turning off halts new decisions and in-flight builds but leaves existing Cabins untouched (no destructive path on disable). Every action is recorded in a decision log with the input-score snapshot. |
| PO9 | Observability: `SHOW CABIN_OPTIMIZER` — per managed Cabin: state, net-benefit score, hint hit rate, coverage, pages, last action + reason; production counters per action type and budget utilization; ANALYZE's Cabin-hit output carries `cabin_optimizer=true` on an optimizer-managed Cabin. |
| PO10 | Deterministic testing: seed-driven statistics streams replayed through the pure decision core reproduce identical action sequences. Structural requirement on the code: **decide (pure, side-effect free) and execute (effectful) are separate phases**. Oracles: no oscillation under stationary workloads, budget invariant, disable-switch harmlessness. |

## II.3 Architecture

```
            (one, on core 0's cadence)
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
  refused, since a zero window prices every Cabin free), chosen so a
  Cabin survives a market overnight (~105 half-lives at the default 600 s;
  survival is log₂(B/C) + 2×T_amort half-lives of silence) where a window
  of 1 is a nightly rebuild loop (a business-days scenario against
  `bench/` at `1769487`). The window is **one belief read by both
  sides**: raising it lowers the admission bar by the same factor, and
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
- **DECAYING**: no EXTEND/HEAL spending; the sets remain; recoverable.
- **DROPPED**: sets discarded, catalog state removed, decision logged.
  Re-nomination starts from scratch as CANDIDATE.

Jurisdiction invariant: the cabin optimizer enumerates only Observational
Cabins it created (`kCabinOriginAuto` in the Cabin catalog state). Bound
Cabins and manually-declared Cabins are invisible to it.

## II.6 Configuration surface

| Setting | Default | Notes |
|---|---|---|
| `cabin_optimizer` | off (experimental) | runtime switch (`SET`), non-destructive off (PO8) |
| `cabin_optimizer_page_budget` | 1024 pages, the instance's | PO6 |
| `cabin_optimizer_theta_create_pct` / `_drop_pct` / `_swap_pct` / `_extend_pct` / `_heal_pct` | 300 / 50 / 200 / 20 / 10 | percent integers; validated against the hysteresis gap |
| `cabin_optimizer_confirm_snapshots` | 3 | N_confirm |
| `cabin_optimizer_amort_windows` | 64 half-lives | T_amort — the build-cost amortization window (§II.4); 0 refused |
| `cabin_optimizer_cooldown_half_lives` | 128 half-lives | T_cooldown — the DECAYING dwell (§II.4); 0 accepted; cannot usefully sit below a workload's longest quiet period |
| `cabin_optimizer_snapshot_interval_ms` | 10,000 | decision cadence |

## II.7 Non-goals

The cabin optimizer does not relocate or cluster tuples (§6); does not manage Bound Cabins (assertion-owned,
permanently outside its jurisdiction); takes no buffer-pool-miss signal,
runs no per-core controller (PO4: one for the instance), and uses no
learned decision model (a learned
model conflicts with the determinism contract); creates and drops no
index — its vocabulary is Cabins only; and accepts no user-facing hint
(`PIN`, `FORBID`).
