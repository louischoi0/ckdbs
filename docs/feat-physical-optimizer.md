# Physical Optimizer v1 — Autonomous Advisory Cabin Management

Status: **ADOPTED (experimental)**
Related documents: `cabin.md` (Observational class; Bound class out of
scope), `assertion.md` §5, `eviction.md` (EV3 evictability, EV6 scan ring),
`scheduler.md` (background group), `analyze.md`, `testing-workplan.md`.

---

## 1. Positioning

Physical optimizer v1 is the first realization of the self-managing-storage
vision: the engine observing its own workload and deciding its own physical
structures. v1 deliberately excludes heap tuple relocation — that path
entangles heap integrity, MVCC, and WAL, and offers no safe failure mode.
Instead, v1 operates exclusively on **Observational (advisory) Cabins**,
which have the decisive property that a wrong decision costs performance
only, never correctness: stale hints heal on read, dangling entries are
discarded, and a dropped Cabin merely returns the system to its baseline.

The component carries the plain technical name — **physical optimizer**
(class `PhysicalOptimizer`) — rather than a frontier-lineage codename; it
is engine machinery, not a user-facing storage concept like Keystone,
Waystone, or Cabin.

The physical optimizer is a per-core background controller that consumes workload
statistics and issues a closed vocabulary of actions over Observational
Cabins. It is experimental in v1, runtime-switchable, and every decision it
makes is logged with the inputs that produced it.

## 2. Decision Record

| ID | Decision |
|----|----------|
| PO1 | Action vocabulary (closed set): **CREATE** (build a new Cabin for a column combination), **EXTEND** (widen an existing Cabin's value coverage), **HEAL** (batch re-validate location hints), **DROP** (retire a cold or unhealable Cabin). REBUILD is excluded (≡ DROP+CREATE). **Bound Cabins are outside the physical optimizer's jurisdiction** — owned by assertions, never read, never touched (invariant, debug-asserted). |
| PO2 | Input signals, exactly three: **(S1)** fingerprint execution frequency under the R1 lazy exponential-decay score; **(S2)** observed predicate scan cost — pages scanned per execution, from the executor's per-statement counters; **(S3)** Cabin quality — hint hit/failure counters and lookup coverage misses. Buffer-pool miss statistics are deferred to v2 (relation-granular, too coarse for column/value decisions). |
| PO3 | Decision model: **cost–benefit formula** (§4). Determinism requirements: the decision core is a **pure function** from a statistics snapshot to an action set, computed in **fixed-point integer arithmetic** (no floats), with hysteresis built in as asymmetric margin factors and cooldowns — a raw cost model oscillates; the margins are load-bearing, not tuning sugar. |
| PO4 | Execution: a background-group task on each relation's **home core**. Independent decisions per core; no cross-core coordination (EV4 spirit). All build/extend scans go through the **scan ring (EV6)** — mandatory, so the physical optimizer can never displace the foreground working set it is trying to serve. |
| PO5 | Lifecycle state machine per managed Cabin: `CANDIDATE → BUILDING → ACTIVE → DECAYING → DROPPED`, with `DECAYING → ACTIVE` recovery on score rebound and `BUILDING → discard` on failure/interruption. All transitions execute as single home-core steps (atomicity practice established by the AST06 cutover). |
| PO6 | Budget: per-core page budget for optimizer-managed Cabins (`kds.po_page_budget`). Over-budget CREATE is admitted only in **exchange** for dropping the lowest-net-benefit ACTIVE Cabin (explicit replacement rule — optimization within a budget, not open-ended growth). Memory residency is the buffer pool's concern (Observational Cabin pages are evictable, EV3); this budget governs disk and upkeep. |
| PO7 | Refresh strategy: quality surveillance, not eager maintenance. Hint-failure rate above threshold ⇒ HEAL; if quality does not recover after HEAL (e.g., mass relocation by bulk UPDATE) ⇒ DROP — demand, if real, re-nominates the candidate. "Discard and re-observe" over "repair at any cost" is the correct posture for advisory structures. |
| PO8 | Safety: experimental status; runtime kill switch `SET kds.physical_optimizer = on|off`. Turning off halts new decisions and in-flight builds but leaves existing Cabins untouched (no destructive path on disable). Every action is recorded in a decision log with the input-score snapshot. |
| PO9 | Observability: `sys.physical_optimizer` view (per managed Cabin: state, net-benefit score, hint hit rate, coverage, pages, last action + reason); production counters per action type and budget utilization; ANALYZE Cabin-hit output gains a flag marking optimizer-managed Cabins. |
| PO10 | Deterministic testing: seed-driven statistics streams replayed through the pure decision core reproduce identical action sequences. Structural requirement on the code: **decide (pure, side-effect free) and execute (effectful) are separate phases**. Oracles: no oscillation under stationary workloads, budget invariant, disable-switch harmlessness. |

---

## 3. Architecture

```
            (per home core)
  ┌─────────────────────────────────────────┐
  │  Stats collectors (S1,S2,S3) ──► Snapshot│
  │                                     │    │
  │                (pure, fixed-point)  ▼    │
  │            PhysicalOptimizer::Decide(Snapshot)    │
  │                     │ ActionSet          │
  │                     ▼                    │
  │            PhysicalOptimizer::Execute ──► Cabin   │
  │             (background task,    machinery│
  │              scan ring, single-  + decision│
  │              step transitions)     log   │
  └─────────────────────────────────────────┘
```

- **Snapshot**: an immutable, versioned aggregation of S1–S3 taken at
  decision time. Snapshot construction is the only stats read; Decide never
  reads live counters (determinism).
- **Decide**: pure function `Snapshot → ActionSet` implementing §4. No
  allocation of engine resources, no I/O, no clock reads (the decay epoch
  is part of the snapshot).
- **Execute**: applies actions with the machinery constraints of PO4/PO5.
  Interruptible between cooperative batches; an interrupted BUILDING is
  discarded, never resumed half-built.

## 4. Cost–benefit model (PO3)

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
  pages) amortized over window `T_amort` (PROPOSED: the R1 decay
  half-life, so build cost and benefit decay on the same clock);
- `h_fail` — hint failure rate (S3), `f_lookup` — decayed lookup
  frequency, `k_heal` — pages per heal event (PROPOSED 2).

**Rules** (asymmetric margins = hysteresis; all thresholds PROPOSED,
configuration-surfaced):

| Action | Condition |
|---|---|
| CREATE (CANDIDATE→BUILDING) | `B > θ_create × C` sustained for `N_confirm` consecutive snapshots (θ_create = 3, N_confirm = 3) and budget admits (or replacement rule fires) |
| EXTEND | Cabin ACTIVE and coverage-miss share of lookups > `θ_extend` (= 20%) and the missed share's marginal `B` alone clears `θ_create × ΔC` |
| HEAL | `h_fail > θ_heal` (= 10%) while `B > θ_drop × C` |
| DECAYING (ACTIVE→) | `B < θ_drop × C` (θ_drop = 0.5) |
| DROP (DECAYING→) | condition persists for `T_cooldown` (= 2 × T_amort) — or HEAL already attempted without quality recovery (PO7) |
| recover (DECAYING→ACTIVE) | `B > θ_create × C` again |

The wide gap θ_drop ≪ 1 ≪ θ_create plus `N_confirm`/`T_cooldown` is the
anti-thrash mechanism: a Cabin is created only on strong sustained evidence
and retired only on strong sustained absence of it.

**Budget arbitration (PO6).** If CREATE is justified but the budget is
full: evict the ACTIVE Cabin with the minimum `B − C` iff the candidate's
`B − C` exceeds it by factor `θ_swap` (PROPOSED 2). Otherwise the candidate
waits. This makes the budget a solved ranking problem, not a growth valve.

## 5. Lifecycle details (PO5)

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

Jurisdiction invariant: the physical optimizer enumerates only Observational Cabins it
created (ownership tag in the Cabin catalog state). Bound Cabins and any
future manually-declared Cabins are invisible to it.

## 6. Configuration surface

| Setting | Default (PROPOSED) | Notes |
|---|---|---|
| `kds.physical_optimizer` | off (experimental) | runtime switch, non-destructive off |
| `kds.po_page_budget` | pool/8 (disk pages, per core) | PO6 |
| `kds.po_theta_create` / `_drop` / `_swap` / `_extend` / `_heal` | 3 / 0.5 / 2 / 0.2 / 0.1 | fixed-point |
| `kds.po_confirm_snapshots` | 3 | N_confirm |
| `kds.po_amort_window` | R1 half-life | T_amort |
| `kds.po_snapshot_interval` | 10 s | decision cadence |

## 7. Non-goals (v1)

- Heap tuple relocation / clustering — explicitly out (the headline scope
  decision of v1).
- Bound Cabin management — assertion-owned, permanently out of the physical optimizer's
  jurisdiction.
- Buffer-pool-miss-driven signals, cross-core coordination, learned
  decision models — deferred (v2 candidates; learned models additionally
  conflict with the determinism contract and need a separate decision).
- Index (B+tree) creation/dropping — the physical optimizer's vocabulary is Cabins only
  in v1; auto-indexing is a far larger contract surface.
- User-facing hints to steer the physical optimizer (`PIN`, `FORBID`) — reserved grammar
  space, not in v1.
