# Cabin controller

The **`CABIN AUTO` promotion pipeline** `cabin.md` §8.1 names: one
background controller for the instance that creates, extends, heals and
drops Observational Cabins from the engine's own access signals. Built and
experimental, **off by default** (`cabin_optimizer`).

This file was Part II of `physical-optimizer.md` (last at `418670a3`) until that file was split
(2026-10-22); every id is unchanged — decisions `PO1`-`PO10` and R2,
sections `§II.n` — so existing citations resolve here. The lazy-decay
score it reads (R1) is `decay.md`'s, and the page epoch its Cabins' hints
carry (R4) is `heap-and-tuple.md` §3.1a's.

The component's runtime name is `CabinOptimizer`: class `CabinOptimizer`,
config keys `cabin_optimizer*` (§II.6), view `SHOW CABIN_OPTIMIZER` (a
`SHOW` rather than a `sys.*` relation, for `SHOW ASSERTIONS`' reason: the
dispatcher holds the controller, the executor and the collector, and a
`sys.*` SELECT path holds none of them).

Markers: `[CONFIRMED]` is settled; `[PROPOSED]` marks a default that
shipped as written and is amendable by measurement; `[OPEN]` must not be
assumed.

**What is built.** The signal plumbing and snapshot
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

---

## II.1 Positioning

The engine's second purpose is acceleration that can be dropped without
changing a result (`/overview.md`, "Droppable without changing a
result"). The Cabin controller is that acceleration deciding itself: the engine reads its own workload and
chooses which Cabins to keep. What makes the engine's own judgement safe
here is the structure it acts on, not the controller.

- **A Cabin is authoritative for the values it observed** (`cabin.md`
  C1): a set is complete for each observed value and maintained on every
  write that moves its input, so a served answer is the walk's answer.
  A value never observed is walked.
- **Dropping a Cabin returns its queries to the tree walk.** Every action
  the controller takes — a CREATE, an EXTEND, a HEAL, a DROP — changes
  which path answers, never what it answers; a wrong decision costs
  performance only. A stale location hint is verified and healed on read,
  a dangling entry is discarded.
- **It acts on Observational Cabins it created, and nothing else.** Bound
  Cabins (assertion-owned) and declared Cabins are outside its
  jurisdiction (PO1, §II.5).
- **It builds; it does not choose a statement's arm** (PO11). A Cabin it
  created is read through only by a statement whose administrator's plan
  names it, so switching the controller on changes how no statement runs.
- **It moves no tuple.** No relayout mover exists (`heap-and-tuple.md`
  §7.1); a move entangles page integrity, MVCC and the WAL.

It is one background controller for the instance, issuing a closed
vocabulary of actions; experimental, runtime-switchable, and every
decision is logged with the inputs that produced it.

## II.2 Decision Record

| ID | Decision |
|----|----------|
| R2 | **Inputs are the existing collectors only**: the Cabin controller reads its own signal collector (`stats/optimizer_signals.hpp`, fed at statement completion beside the access-shape and trail collectors) and the Cabin store's counters; it does not read `sys.access_stats`. An input it lacks becomes a collection change in the layer that owns collection, spec'd there first. **Affinity is not an input** (AR0-5 D18): D18 keeps a relation's affinity as a statistic with a weight-0 consumer (AR0 D10) until AS-E; nothing stores it (`sys.tables.owner_core` is a reserved word, written 0, never read), and the declaration a consumer would read is the namespace a relation was created in (`namespace.md` NS10), already on its `sys.tables` row. |
| PO1 | Action vocabulary (closed set): **CREATE** (build a new Cabin for a column combination), **EXTEND** (widen an existing Cabin's value coverage), **HEAL** (batch re-validate location hints), **DROP** (retire a cold or unhealable Cabin). REBUILD is excluded (≡ DROP+CREATE). **Bound Cabins are outside the cabin optimizer's jurisdiction** — owned by assertions, never read, never touched (invariant, debug-asserted). |
| PO2 | Input signals, exactly three: **(S1)** fingerprint execution frequency under the R1 lazy exponential-decay score (`decay.md`; `stats/decay.hpp` — one decay implementation, shared); **(S2)** observed predicate scan cost — pages scanned per execution, from the executor's per-statement counters; **(S3)** Cabin quality — hint hit/failure counters and lookup coverage misses. Buffer-pool miss statistics are not an input (relation-granular, too coarse for column/value decisions). |
| PO3 | Decision model: **cost–benefit formula** (§II.4). Determinism requirements: the decision core is a **pure function** from a statistics snapshot to an action set, computed in **fixed-point integer arithmetic** (no floats), with hysteresis built in as asymmetric margin factors and cooldowns — a raw cost model oscillates; the margins are load-bearing, not tuning sugar. |
| PO4 | Execution: **one controller for the instance**, ticked by a background-group task on core 0's cadence (AT-S8 step 3: every core's dispatcher is handed the one `OptimizerSurface` - the switch, the collector every core feeds, and the board the tick publishes the controller's view to). **A build announces each seed's set before its scan and asks the banking gate** (AY-S6; `cabin.md` §6, §6a), as the serve path's build does, so a write another core lands behind the walk is in the set it banks - which the foreign key's clearing return relies on (`foreign-keys.md` §3a). Until AY-S6 it walked and then committed, and such a write was lost. All build/extend scans go through the **scan ring (EV6)** — mandatory, so the cabin optimizer can never displace the foreground working set it is trying to serve. |
| PO5 | Lifecycle state machine per managed Cabin: `CANDIDATE → BUILDING → ACTIVE → DECAYING → DROPPED`, with `DECAYING → ACTIVE` recovery on score rebound and `BUILDING → discard` on failure/interruption. All transitions execute as single steps of the controller's tick. |
| PO6 | Budget: one page budget for the instance's optimizer-managed Cabins (`cabin_optimizer_page_budget`). Over-budget CREATE is admitted only in **exchange** for dropping the lowest-net-benefit ACTIVE Cabin (explicit replacement rule — optimization within a budget, not open-ended growth). Observational entry sets are memory-resident (`cabin.md` §9), so a Cabin's "pages" are a pricing proxy, 1 + entries / 254 (`CabinOptimizerExecutor::PagesProxyOf`); the budget governs that size and upkeep. |
| PO7b | **The candidate a fingerprint names is read in written-position order since CC-Q15** (`instructions/v3.2.0/workorder-cc-plan-statistics.md`): the first column Cabin probe, else the first equality filter scan, by step id - which since CB-Q3 is the relation's written position - so the join planner's executed order does not choose among them. The planner's order can still change a step's kind (a relation probed in one order is filter-scanned in another), and with it the candidate; the Cabin controller's other inputs are unchanged. |
| PO7c | **An execution signal is a fingerprint's and its candidate's together** (DI-S2, `instructions/v3.2.0/workorder-di-shape-identity.md` DI-Q12 (a); `stats/optimizer_signals.hpp`, `SignalKey`). A fingerprint is a hash of the statement's text and holds no namespace and no relation oid, so one fingerprint is two statements wherever a name in it resolves to two relations - under two namespaces (`USE`), or over a relation dropped and created again. The signal's key is therefore `(pattern_id, the candidate's relation, the candidate's column)`: each `(relation, column)` is credited the executions that targeted it and no others, and an execution with no candidacy is the fingerprint's own signal, priced by nothing - before DI-S2 it was added to the frequency and the mean of the candidate the fingerprint had last named. The Cabin serving a candidate is the one the newest execution saw and is no part of the key. The 4,096 cap bounds signals, not fingerprints. **A wait sample (S4) is a fingerprint's, not a candidate's** - the sampler knows the session's published pattern and nothing the statement bound - so it is kept beside the fingerprint's signals, credited to none, and read by nothing that decides. What the key does not hold is the column's identity past its position: a position `ADD COLUMN` reuses after `PURGE DROPPED COLUMNS` inherits the dropped column's decayed heat until it decays (`known-gaps.md`). |
| PO7 | Refresh strategy: quality surveillance, not eager maintenance. Hint-failure rate above threshold ⇒ HEAL; if quality does not recover after HEAL (e.g., mass relocation by bulk UPDATE) ⇒ DROP — demand, if real, re-nominates the candidate. "Discard and re-observe" over "repair at any cost" is the correct posture for a structure whose drop returns its queries to the tree walk. |
| PO8 | Safety: experimental status; runtime kill switch `SET cabin_optimizer = on\|off`. Turning off halts new decisions and in-flight builds but leaves existing Cabins untouched (no destructive path on disable). Every action is recorded in a decision log with the input-score snapshot. |
| PO9 | Observability: `SHOW CABIN_OPTIMIZER` — per managed Cabin: state, net-benefit score, hint hit rate, coverage, pages, last action + reason; production counters per action type and budget utilization; ANALYZE's Cabin-hit output carries `cabin_optimizer=true` on an optimizer-managed Cabin - a step that carries it is a pinned one (`arm=plan`, PO11). |
| PO12 | **Collection queues no statement, and an off controller collects nothing** (`docs/rules/rules.md` §3a; `instructions/v3.2.0/workorder-dl-statistics-queue-no-statement.md` DL-S1). **Off** - the default - no statement builds an identity or a candidate for the collector, publishes a pattern id, notes an execution, or forwards a Cabin probe or hint: what it pays is a relaxed load of the switch at each place that would otherwise work for it. **Where no tick can run** - `cabins = off`, or `cabin_optimizer_snapshot_interval_ms = 0` - there is no collector at all, whatever the switch says: nothing would ever fold a note. Turning the controller on starts it from what was folded before it was last turned off, decayed by the clock, and from nothing if it never ran (PO8 keeps existing Cabins; this keeps no fabricated count). **On**, a note writes its own core's part of the collector - two fixed tables, 1,024 shapes and 256 Cabins live between two ticks, allocated at the core's first note - under a latch it only tries; the tick folds the parts in core order and entries by id into the state `Decide` reads (PO10's determinism: the same notes in the same per-core order give the same snapshot), and the fold is where a full table evicts: room for a part's newcomers is made in one pass, the coldest going together, a tie to the lower id. A note that meets the fold's copy, or finds its part full, is dropped and counted (`sys.meta`'s `cabin_optimizer_notes_busy`, `cabin_optimizer_notes_full`). A part's decayed pairs merge into the folded ones exactly at whole half-lives (`MergeDecay`, `decay.md`). **The controller's views read a published copy** (DL-S3): the tick, when it is done, publishes what `SHOW CABIN_OPTIMIZER` and `SHOW META`'s `cabin_optimizer` section show - the managed table, the decision log, the counters, each Cabin's quality as the collector's folded state then stood - and a view on any core reads that copy, so it waits for no tick and no Cabin's build; the columns are as of the last tick that ran, and before the first the table is empty. The Cabin store's own counts (`SHOW CABINS`' `hits` and `misses`, `sys.meta`'s `cabins` section) have no switch and take no latch: a core's own words, summed when read. |
| PO11 | **A controller-made Cabin serves a read only through a plan that names it** (CX-Q6 (a), `instructions/v3.2.0/workorder-cx-managed-plans.md` CX-S3b; `parser-v2.md` I12). The access ladder tries a Cabin of `auto` origin for no step unless an enabled administrator's plan pins that step `ACCESS <relation> CABIN (<column>)`; `CABIN` unnamed admits declared Cabins alone. So a CREATE moves no statement's arm, under every `join_order`: the fingerprint that earned it keeps its filter scan - and keeps naming the column as its candidate (PO7b), which the managed entry already answers - until a DBA or an agent names the Cabin in a plan (`CREATE PLAN ... FOR <the statement> VARIANT ... ORDER (...) ACCESS <relation> CABIN (<column>)`, a one-relation statement included). **An unnamed Cabin is probed by no statement's step**, so it observes no value, sights none for an EXTEND to seed from, and its write hook finds no observed value to maintain: it holds its catalog row and its budget page until it is named or decays. Only the foreign key's reverse check (below) asks it, and its misses count toward the coverage share that triggers an EXTEND, which then finds nothing sighted. **An enabled plan that names a Cabin RESTRICTs its DROP** (CX-Q4 (a)): the executor skips the action and counts it (`cabin_optimizer_drops_restricted`), the entry staying as the decision left it (DECAYING; ACTIVE after a quality-collapse DROP, PO7) and asked again at a later tick; a budget swap (PO6) whose victim is so restricted builds nothing - its CREATE is answered as a failed build and the candidate waits. The foreign key's reverse check reads a child's Cabin whoever made it (`foreign-keys.md` §3a): it is an enforcement path, not a statement's plan. |
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
16.16. Per candidate or managed Cabin `c` over the execution signals that
name it (PO7c: a fingerprint's executions that targeted `c`):

**Benefit** — decayed page savings per unit time:

```
B(c) = Σ_i  f_i × max(0, P_scan,i − P_cabin)
```

- `f_i` — R1-decayed execution frequency of signal `i`, a fingerprint's
  executions whose predicate is served by `c` (S1);
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
after ~16 half-lives, and the decision core consults `Log2ValueAt` (`decay.md` R1)
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
- **ACTIVE**: serving; quality counters accumulate.
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

The Cabin controller does not relocate or cluster tuples
(`heap-and-tuple.md` §7.1); does not manage Bound Cabins (assertion-owned,
permanently outside its jurisdiction); takes no buffer-pool-miss signal,
runs no per-core controller (PO4: one for the instance), and uses no
learned decision model (a learned model conflicts with the determinism
contract); creates and drops no index — its vocabulary is Cabins only; and
accepts no user-facing hint (`PIN`, `FORBID`).
