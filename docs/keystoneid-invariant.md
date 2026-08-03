# Keystone id — issue-once invariant (concept + workplan)

Status: **DECIDED** (K1–K5 below). Milestones K-M1..K-M6 not started.
Depends on: Keystone super-column contract (40-bit id + 8-bit flags +
16-bit meta id), per-relation catalog metadata, WAL, core-ownership
dispatch.
This document deliberately covers the engine-level id contract only;
features that *consume* the invariant keep their own specs.

Decisions fixed here:

- **K1 — Issue-once.** A Keystone id is issued to exactly one tuple in
  the lifetime of a relation. It is never rebound to another tuple, by
  any path: not through the allocator, not through delete-then-insert,
  not through crash recovery.
- **K2 — Immutable.** A tuple's Keystone id never changes after
  insert. An UPDATE that targets the super column is **Unsupported**
  (V5-style hard rejection, no slow path).
- **K3 — No ordering promise.** Issue-once does *not* imply
  monotonicity or density. Gaps are legal and expected (bump-ahead
  recovery, aborted inserts). Nothing in the engine may rely on ids
  being sequential or contiguous.
- **K4 — Lifetime budget is a documented product constraint.** 2^40
  ids per relation is the relation's lifetime insert budget, stated
  openly in product docs rather than engineered around.
- **K5 — Offline re-key reserved.** The one sanctioned way to reset
  the budget is an explicit offline maintenance operation that
  consciously re-issues ids under an exclusive window. Reserved, not
  specified here.

---

## 1. The invariant and why it earns its place

> **Keystone ids are issued once, never rebound, never mutated.**

What this buys, engine-wide:

1. **Snapshot-safe pk resolution.** Without issue-once, a pk freed and
   re-issued while an old snapshot can still see the prior tuple makes
   plain pk lookups resolve to the wrong incarnation — the reader
   walks the *new* tuple's undo chain and silently misses a row its
   snapshot is entitled to. Issue-once deletes the hazard structurally
   instead of gating it behind a purge-horizon rule that every future
   feature would have to re-prove.
2. **(oid, pk) becomes a forever-unique key.** Every structure keyed on
   it — the statistics primitives, waystone trail entries, in-memory
   canonical caches, any future replication or change feed — gets
   identity for free: a stored (oid, pk) can dangle, but it can never
   mis-attribute. "Dangling ⇒ skip" becomes a universally sound rule.
3. **Audit posture.** For the finance-adjacent positioning: "a row's
   identifier never changes and is never reissued" is a compliance
   sentence, not just an implementation detail. Immutable, unique-for-
   all-time record identity is a precondition for defensible audit
   trails.
4. **Simpler invalidation everywhere.** Validation logic that today
   would need epoch-style incarnation checks on ids reduces to
   existence + visibility checks.

What it deliberately does **not** promise (K3): no monotonic order, no
gap-freeness, no correlation between id order and insert order across
crashes. The min_key semi-sorted heap keys off values, not issuance
order, and remains unaffected.

## 2. Allocator contract

Per relation, the allocator maintains a persisted **high-water mark**
(HWM): the smallest id never yet issued. Rules:

- Issue = return current cursor, advance. The cursor never moves
  backward, and no free-list of any kind exists for Keystone ids.
- **Bump-ahead persistence** `[PROPOSED]`: the HWM is persisted in
  chunks — the durable record always holds a *ceiling* at or above
  every id actually issued (persist `cursor + N`, hand out ids up to
  it from memory, persist the next chunk when exhausted). Crash
  recovery resumes from the persisted ceiling; the skipped remainder
  of the chunk becomes a gap, which K3 makes legal. This keeps the
  hot path free of per-insert durability cost.
- Chunk size `N` `[PROPOSED]`: fixed global constant (e.g. 4096),
  frozen in the superblock like `kds.inline_cell_width`. Not
  per-relation tunable in v1 — one less knob.
- Persistence location `[PROPOSED]`: the relation's catalog metadata
  row, updated through the normal logged catalog write path (the
  bump-ahead cadence makes the log traffic negligible).
- Concurrency: the relation's owning core is the only issuer
  (core-ownership dispatch), so the allocator is single-writer by
  construction — no atomics, no cross-core coordination.
- Explicit-id inserts, if any path allows them, must go through the
  same gate: an explicit id ≥ HWM advances the HWM past it; an
  explicit id < HWM is rejected (it may collide with an issued id,
  and proving otherwise would require the free-list this design
  forbids).

## 3. Lifetime budget (K4) — the honest math

Issue-once converts 2^40 (≈ 1.10 × 10^12) from a live-row bound into a
**lifetime issuance budget per relation**, consumed by every insert,
including rolled-back ones and bump-ahead gaps.

| sustained insert rate (one relation) | budget exhausted in |
|---|---|
| 1,000 /s | ~35 years |
| 5,000 /s | ~7 years |
| 50,000 /s | ~8 months |
| 500,000 /s | ~25 days |

Product-doc stance: for master/account-class relations the budget is
effectively unlimited; for high-rate ingest relations (trade/event
logs) it is reachable and must be planned for. The sanctioned
patterns, in order:

1. **Relation partitioning by period** (monthly/quarterly log
   relations) — already standard OLTP operational practice; each
   partition gets its own 2^40.
2. **Offline re-key** (K5) as the escape hatch when partitioning was
   not applied in time.

Widening the id beyond 40 bits was considered and rejected: it
forfeits the fixed 64-bit super-column word (40+8+16) that the tuple
header, meta-pool handle, and page arithmetic are built on. The
constraint is cheaper than the redesign.

## 4. Offline re-key (K5) — reserved semantics

Not specified in this document; the reservation fixes only its
boundary conditions so nothing else accidentally forecloses it:

- It is an **offline, exclusive** operation on one relation: the
  owning core runs it as a maintenance task with no concurrent
  statements (the scheduling model already provides this exclusivity).
- It deliberately violates K1 **once, atomically, and visibly**:
  every tuple receives a fresh id from a reset HWM; the operation is
  logged as a single recoverable unit.
- Everything keyed on the old (oid, pk) space is invalidated
  wholesale: statistics, waystone trees, canonical caches. All are
  droppable classes by design, so invalidation is a purge, not a
  migration.
- Business keys are unaffected (they live in ordinary columns); only
  engine identity is rewritten. External systems that captured
  Keystone ids must treat re-key as a new epoch — which is why the
  operation is offline, explicit, and expected to be rare.

## 5. Workplan

**K-M1 — Audit current issuance paths.**
Read every path that produces a Keystone id (insert executors,
bootstrap, any recovery path) and every path that could re-issue one
(free-list, crash restart, catalog rebuild). Deliverable: a short
findings note + failing tests that *demonstrate* any reuse that exists
today. Acceptance: reuse behavior of the current engine is documented
fact, not assumption.

**K-M2 — HWM + bump-ahead allocator.**
Implement §2: persisted per-relation HWM, chunked bump-ahead, recovery
resume-from-ceiling, explicit-id gate. Deterministic tests: crash
between chunk persist and issuance (sim-crash via IoBackend seam)
must never re-issue; gaps appear and are harmless. Acceptance: K1
holds across simulated crash/restart cycles; insert hot path shows no
added durability wait.

**K-M3 — Enforce K2 (immutability).**
Compiler/executor: an UPDATE whose SET list touches the super column
returns Unsupported at compile time (J2 policy — no slow path).
Acceptance: negative tests at parser, compiler, and wire levels.

**K-M4 — Budget observability.**
Expose per-relation issued-count and remaining budget (derived from
HWM) via the catalog view / SHOW path, with a health warning at a
threshold `[PROPOSED: 90%]`. Acceptance: an operator can see budget
consumption without arithmetic; crossing the threshold is visible in
SHOW output.

**K-M5 — Documentation promotion.**
Add the invariant to the design-invariants list verbatim ("Keystone
ids are issued once, never rebound, never mutated; pk UPDATE is
Unsupported"), and add the K4 budget table + partitioning guidance to
the product/operations docs. Acceptance: both documents merged;
README's constraint section references the budget honestly.

**K-M6 — (reserved) Re-key operation spec.**
Blocked until a concrete need appears; §4's boundary conditions are
its inputs.

Suggested order: K-M1 → K-M2 → K-M3 (independent of M2, can parallel)
→ K-M4 → K-M5. M1 first is non-negotiable: everything else assumes we
know, rather than believe, what the engine does today.

## 6. Out of scope

- Cabin and any other consumer feature's use of the invariant — their
  own specs cite this document.
- Re-key implementation (K-M6).
- Cross-relation or global id spaces; the id remains per-relation.
- Any ordering/density guarantee (K3 forbids relying on one).
