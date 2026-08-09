# Physical Optimizer v1 — Workplan (PHY01–PHY08)

Status: **READY FOR EXECUTION**
Spec: `physical-optimizer.md` (normative). Related: `cabin.md`, `eviction-workplan.md`
(EVT06 scan ring is a hard dependency of PHY04), `analyze.md`,
`testing-workplan.md`.

Engine rules apply throughout: explicit Status errors, thread-per-core
core-local state, deterministic tests, decide/execute phase separation
(PO10 structural requirement — enforced by construction: the decision core
lives in a module with no engine-effect includes).

---

## PHY01 — Statistics plumbing (S1–S3)

**Scope.** Make the three input signals collectible into a snapshot.

**Deliverables.**
- S1: per-fingerprint R1-decayed execution frequency exposed to snapshot
  construction (reuses the existing decay-score machinery; add per-
  fingerprint accumulation if currently only per-relation).
- S2: executor counter "pages scanned" per statement, aggregated per
  fingerprint (decayed mean). Carried baseline: when a Cabin becomes
  ACTIVE, freeze the pre-Cabin `P_scan` into its state (spec §4).
- S3: Cabin hint hit/failure and coverage-miss counters (some exist via
  hint-heal accounting; unify and expose).
- `Snapshot` type: immutable, versioned, fixed-point fields, decay epoch
  included; construction is a single home-core step.

**Acceptance.** Unit tests for decayed accumulation; snapshot immutability;
counters correct under scripted workloads.

---

## PHY02 — Decision core (pure)

**Scope.** `PhysicalOptimizer::Decide(Snapshot) → ActionSet` implementing spec §4
exactly. **No I/O, no clock, no allocation of engine resources, no float.**

**Deliverables.**
- Fixed-point (16.16) B/C evaluation, rule table (CREATE / EXTEND / HEAL /
  DECAYING / DROP / recover), N_confirm tracking, cooldown tracking,
  budget arbitration with θ_swap replacement selection.
- ActionSet as plain data (target Cabin/candidate id + action + reason
  code + score snapshot) — directly serializable into the decision log.
- Jurisdiction filter: Bound Cabins and Cabins not owned by the optimizer excluded at
  snapshot construction; debug assert in Decide that no such id appears.

**Acceptance.** Pure-function unit tests: golden ActionSets for scripted
snapshot sequences; hysteresis test (stationary noisy workload ⇒ zero
create/drop oscillation over long sequences); budget invariant test
(Σ pages ≤ budget after every ActionSet); arithmetic determinism test
(identical snapshots ⇒ bit-identical ActionSets).

---

## PHY03 — Catalog state and decision log

**Scope.** Persistence of managed-Cabin lifecycle state and the decision
log (PO5, PO8).

**Deliverables.**
- Optimizer lifecycle state per managed Cabin in the Cabin catalog area: lifecycle
  state, ownership tag (optimizer-created), frozen `P_scan` baseline,
  evidence accumulators (N_confirm, cooldown timestamps in decay epochs).
- Decision log: append-only, bounded (ring of last K decisions, PROPOSED
  K = 1024 per core), each entry = ActionSet element + snapshot digest.
  Not WAL-critical (advisory data; loss on crash is acceptable and
  documented — state machine re-derives from re-observation).
- Crash posture: BUILDING at crash ⇒ discarded on startup (no resume);
  ACTIVE Cabins persist as ordinary Observational Cabins do.

**Acceptance.** Restart tests: ACTIVE survives, BUILDING discarded,
CANDIDATE evidence resets cleanly; log ring wraps correctly.

---

## PHY04 — Executor (background task)

**Scope.** `PhysicalOptimizer::Execute(ActionSet)` on the home core's background
group. **Depends on EVT06 (scan ring).**

**Deliverables.**
- Decision cadence: snapshot → Decide → Execute every
  `kds.po_snapshot_interval`, as a cooperative background task.
- CREATE/EXTEND: observation-seeded build through the scan ring,
  cooperative batches, single-step state transitions at start/finish.
- HEAL: batch hint re-validation walk (reuses read-path heal primitive).
- DROP: page reclamation + catalog removal in a single publish-style step.
- Kill switch semantics: `off` checked at batch boundaries; in-flight
  build discarded, nothing destructive (PO8).

**Acceptance.** Deterministic tests: each action end-to-end on small
relations; interruption mid-build discards cleanly; foreground working-set
protection test (build over large relation does not evict scripted hot
set — reuses the EVT06 oracle); disable-switch harmlessness.

---

## PHY05 — Configuration surface

**Scope.** Spec §6 settings wired through boot + runtime (`SET` for the
switch; thresholds boot-time in v1).

**Deliverables.** Settings, validation (theta ordering θ_drop < 1 <
θ_create, budget sanity), introspection visibility.

**Acceptance.** Boot-validation tests; SET on/off runtime toggle test.

---

## PHY06 — Observability (PO9)

**Scope.** `sys.physical_optimizer` view + counters + ANALYZE flag.

**Deliverables.**
- `sys.physical_optimizer`: per managed Cabin — state, B, C, net, hint hit rate,
  coverage-miss rate, pages, last action + reason code + epoch.
- Production counters: actions by type, budget utilization, snapshot
  cadence health.
- ANALYZE: Cabin-hit line gains `physical_optimizer=true` marking for managed
  Cabins.

**Acceptance.** View snapshot tests under PHY04 scenarios; ANALYZE golden
output.

---

## PHY07 — Deterministic replay harness (PO10)

**Scope.** Seed-driven statistics streams replayed through Decide.

**Deliverables.**
- Stream generator: seeded synthetic fingerprint populations with
  configurable drift (rising star, fading star, stationary noise, bulk-
  update quality collapse).
- Replay runner: stream → snapshots → ActionSet trace; golden traces
  checked in.
- Oracles wired: no-oscillation, budget invariant, jurisdiction (no Bound
  Cabin id ever), HEAL-then-DROP path on quality collapse (PO7).

**Acceptance.** Golden traces stable across runs and platforms
(fixed-point determinism proven here); scenario matrix green in CI.

---

## PHY08 — End-to-end validation and close-out

**Scope.** System-level pass + docs.

**Deliverables.**
- E2E: workload with a skewed hot predicate ⇒ the physical optimizer creates a Cabin ⇒
  measured latency/pages-scanned improvement on the hot fingerprint ⇒
  workload shifts ⇒ DECAYING ⇒ DROP. Full lifecycle observed via
  `sys.physical_optimizer` in one scripted run.
- Benchmark note: optimizer-on overhead with zero eligible candidates
  (target: unmeasurable — snapshot cost only); improvement case recorded
  in the perf log.
- Docs cross-check: `cabin.md` gains the ownership-tag mention;
  positioning stays low-key per policy ("experimental self-managing
  advisory structures", no autonomy overclaim).

**Acceptance.** Green E2E in CI; perf log entries; docs consistent.

---

## Dependency graph

```
PHY01 ──► PHY02 ──► PHY03 ──► PHY04 ──► PHY08
              │                 ▲
PHY05 ────────┤                 │ (EVT06 scan ring — hard dep)
PHY06 (needs PHY04) ────────────┘
PHY07 (needs PHY02) ──► PHY08
```

PHY02/PHY07 are pure-code tracks and can run ahead of storage-side work.
PHY04 must wait for EVT06; if the eviction track lags, everything through
PHY03 plus PHY07 can still land and be fully tested.
