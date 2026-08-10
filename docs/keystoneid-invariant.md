# Keystone id — issue-once invariant (concept + workplan)

Status: **DECIDED** (K1–K5 below). **K-M1 done 2026-08-03** —
`docs/keystoneid-k0-findings.md` is what it found, and its four proposed
amendments are **applied here** (2026-08-03): K3's wording, §1's min_key
aside, §5's milestone order, and §1.2's oid claim. Read the findings before
starting K-M2; three of them change what K-M2 is.

**Milestone state (corrected 2026-08-10 — this line read "K-M2..K-M6 not
started", which §5 below already contradicted for K-M4):**
**K-M1 done**, **K-M4 done** (both 2026-08-03), **K-M3 partly** — the
refusal exists and holds, as a dispatcher-level
`ERR primary-key column '<c>' cannot be updated`
(`src/server/command_dispatcher.cpp`), not the compile-time `Unsupported`
K-M3 specifies, so the *behaviour* is built and the *layer and status code*
are not. **K-M2a, K-M2, K-M5, K-M6 not started**, and K-M2 stays blocked on
K-M2a, which is blocked on work in `docs/wal.md`.
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
- **K3 — No density promise.** Gaps are legal and expected
  (bump-ahead recovery, aborted inserts); nothing may rely on ids
  being contiguous. Issue-once does not *by itself* promise ordering
  — but the allocator (§2) never moves its cursor backward, and the
  semi-sorted heap, the clustered btree and range pruning rely on
  that. Removing monotonicity is a separate decision with its own
  blast radius, not a consequence of this one.
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
2. **(oid, pk) becomes a forever-unique key — once the pk half holds.**
   Every structure keyed on it — the statistics primitives, waystone
   trail entries, in-memory canonical caches, any future replication or
   change feed — would get identity for free: a stored (oid, pk) can
   dangle, but it could never mis-attribute, making "dangling ⇒ skip" a
   universally sound rule.

   **The oid half holds as of 2026-08-08.** It did not before:
   `Catalog::GenerateUserOid()` was an in-memory counter seeded at
   `kUserOidStart` and never read back, so every boot re-issued the same
   object oids — no crash required, a clean restart plus one
   `CREATE TABLE` collided, and the new relation's oid resolved through
   `GetSysTableRow` to the *old* relation. It now recovers its position on
   first use from the highest oid `sys.objects` and `sys.columns` carry,
   and `ObjectOidsAreUniqueAcrossABoot` — the inversion of the test that
   used to demonstrate the collision — pins it.

   **So this bullet still states an objective, and the remaining half is
   the pk.** K1 is what is missing now, in the direction §1.1 describes:
   the durable `next_id` can fall behind the log after a crash (K-M2a).
   Nothing keyed on (oid, pk) may be called collision-free until that
   lands — but the failure mode is now a crash rather than a clean
   restart, which is a different and much narrower window.
3. **Audit posture.** For the finance-adjacent positioning: "a row's
   identifier never changes and is never reissued" is a compliance
   sentence, not just an implementation detail. Immutable, unique-for-
   all-time record identity is a precondition for defensible audit
   trails.
4. **Simpler invalidation everywhere.** Validation logic that today
   would need epoch-style incarnation checks on ids reduces to
   existence + visibility checks.

What it deliberately does **not** promise (K3): no gap-freeness, and no
correlation between id order and insert order across crashes.

It does not promise **ordering** either — but the engine has it anyway,
from §2's allocator, and four things already depend on it. Worth naming,
because the earlier wording here claimed the opposite:

- the semi-sorted heap chain refuses an id below the tail page's
  `min_key` (`heap_chain.hpp`), which is invariant 3 enforced at the one
  place tuples enter — so it depends on *issuance* order, not only on
  values;
- the clustered btree **refuses a non-monotonic id outright**, with
  `OutOfSpace` naming the open split-policy decision rather than guessing
  (`btree.hpp`) — a hard failure, not a lost optimization, and the
  strongest of the four;
- `keystone.hpp` derives uniqueness *from* monotonicity ("unique and
  monotonic by construction rather than by a uniqueness check");
- `kRange`'s `min_key` tail pruning stops a walk on the strength of it
  (`src/exec/step_vm.cpp`).

None of that is a promise this document makes. All of it is a promise
something would have to re-make before monotonicity could be dropped.

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
- Chunk size `N` `[PROPOSED]`: fixed global constant, **4096, and that
  is a floor rather than an example** — measured, not picked
  (`bench/results-keystone-alloc.md`). At 4096 a crash-safe allocator
  costs 1.24× today's; at 64 it costs 43×, which is a 3× INSERT
  regression, because one fsync per 64 rows is still one fsync every 64
  rows. Frozen in the superblock like `kds.inline_cell_width`. Not
  per-relation tunable in v1 — one less knob.
- Persistence location `[PROPOSED]`: the relation's catalog metadata
  row, updated through the normal logged catalog write path — **which
  does not exist**. Catalog rows are unlogged and reach the platter only
  at a checkpoint, which is why K1 breaks across a crash today
  (`keystoneid-k0-findings.md` §4) and what K-M2a is for. The reasoning
  that made this location cheap still holds once it does exist: the
  bump-ahead cadence makes the log traffic negligible.
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

**K-M1 — Audit current issuance paths. DONE 2026-08-03.**
Read every path that produces a Keystone id (insert executors,
bootstrap, any recovery path) and every path that could re-issue one
(free-list, crash restart, catalog rebuild). Deliverable: a short
findings note + failing tests that *demonstrate* any reuse that exists
today. Acceptance: reuse behavior of the current engine is documented
fact, not assumption.

Delivered as `docs/keystoneid-k0-findings.md`, `tests/keystone_id_test.cpp`
and `bench/results-keystone-alloc.md`. Headline: **K1 does not hold across a
crash**, because the durable log names ids that the unlogged `next_id` has
forgotten — a durability problem, not an allocator one, which K-M2 cannot
close alone. The demonstrating tests are green rather than red on purpose;
each names the condition under which it must be inverted, on the grounds
that a permanently-red test is one that gets ignored.

**K-M2 — HWM + bump-ahead allocator. BLOCKED on K-M2a.**
Implement §2: persisted per-relation HWM, chunked bump-ahead, recovery
resume-from-ceiling, explicit-id gate. Deterministic tests: crash
between chunk persist and issuance (sim-crash via IoBackend seam)
must never re-issue; gaps appear and are harmless.

Acceptance, restated after K-M1 — the original wording claimed a crash
property this milestone cannot deliver on its own, at any chunk size:

- the allocator issues from an in-memory interval and touches the
  catalog row once per chunk rather than once per id;
- a restart resumes from the persisted ceiling, never below it, and the
  skipped remainder appears as a gap;
- an explicit id below the HWM is rejected and one at or above it
  advances the HWM past it;
- the insert hot path adds no per-id durability wait;
- **K1 across a crash is K-M2a's criterion, not this one.** With the
  ceiling written through today's unlogged catalog path, this milestone
  can only promise "never re-issues *given* that the ceiling reached the
  platter" — a conditional, and the condition is false today.

**K-M2a — Make the ceiling durable.** §2 persists through "the normal
logged catalog write path". There is no logged catalog write path:
catalog rows are unlogged and reach the platter only at a checkpoint,
which is exactly why K1 breaks across a crash (`keystoneid-k0-findings.md`
§4). Closing it needs a `sys.tables` write that is logged, and recovery to
read it back. Both belong to `docs/wal.md`; they are named here because
without them K-M2 is a performance change wearing a correctness label.

Real order: **logged catalog writes → recovery → K-M2**.

Measured inputs from K-M1 (`bench/results-keystone-alloc.md`), which
decide two things K-M2 would otherwise guess at:

- The allocator is **4.3–4.9% of an unlogged INSERT**. That is the
  ceiling on what this milestone can win, so it is a correctness change
  and must not be sold as a performance one.
- **`N` is settled at 4096 by measurement**, and the `[PROPOSED]` on it
  becomes a **floor rather than a default**. Crash-safe bump-ahead costs
  1.24× today's allocator at N=4096 and 43× at N=64 — a 3× INSERT
  regression, because one fsync per 64 rows is still one fsync every 64
  rows. Forcing durability *per id* instead costs 2629×, capping INSERT
  at ~949/s: that is the shape a crash-safe implementation reaches for
  when it skips bump-ahead, and it is the one outcome to design against.

**K-M3 — Enforce K2 (immutability). PARTLY BUILT.**
Compiler/executor: an UPDATE whose SET list touches the super column
returns Unsupported at compile time (J2 policy — no slow path).
Acceptance: negative tests at parser, compiler, and wire levels.

*What exists (verified 2026-08-10):* the SET list is checked in
`CommandDispatcher`'s UPDATE path and answers
`ERR primary-key column '<c>' cannot be updated`. So K2 holds — no path
mutates a Keystone id — but two clauses of this milestone do not: the
check is at the **dispatcher**, not the compiler, and the code is a plain
error rather than `kUnsupported`. Closing it is moving one check and
changing one status code; the acceptance tests at compiler and wire level
are what would pin it.

**K-M4 — Budget observability. DONE 2026-08-03.**
Expose per-relation issued-count and remaining budget (derived from
HWM) via the catalog view / SHOW path, with a health warning at a
threshold `[PROPOSED: 90%]`. Acceptance: an operator can see budget
consumption without arithmetic; crossing the threshold is visible in
SHOW output.

Two surfaces: **`SHOW BUDGET`** lists every relation with a summary line
carrying `warning=<n>`/`exhausted=<n>`, so the second acceptance clause
holds without reading every row; and **`DESCRIBE`** gains `ids_issued`,
`ids_remaining` and `budget_used` beside the `next_id` they derive from,
which is where someone already looks. `docs/client-manual.md` has both.

The arithmetic is `catalog::BudgetOf()`
(`include/kds/catalog/keystone_budget.hpp`), a pure function of one
integer rather than a line of `<<` in the dispatcher — which is what makes
this milestone survive K-M2: the *source* becomes a persisted HWM, and
none of the arithmetic moves. Three things it settles that an inline
subtraction gets wrong: **issued counts ids spent, not rows living** (a
burned id is spent, and a renderer saying "rows" would be lying);
capacity is `kMaxKeystoneId − kFirstRowId + 1`, one short of 2^40 because
id 0 is reserved; and exhaustion is a flag rather than the tail of a
rounded percentage, since `AllocateRowId` refuses rather than wrapping.

The 90% threshold is **still `[PROPOSED]`** — nothing has argued for a
number, and the honest input is how long a relation takes to cross the
last 10% at its own insert rate (§3's table), which is per-deployment. It
is a named constant, `kKeystoneBudgetWarnFraction`, so moving it is one
edit.

**K-M5 — Documentation promotion.**
Add the invariant to the design-invariants list verbatim ("Keystone
ids are issued once, never rebound, never mutated; pk UPDATE is
Unsupported"), and add the K4 budget table + partitioning guidance to
the product/operations docs. Acceptance: both documents merged;
README's constraint section references the budget honestly.

**K-M6 — (reserved) Re-key operation spec.**
Blocked until a concrete need appears; §4's boundary conditions are
its inputs.

Order, revised after M1:

> **K-M1 (done) → K-M4 (done) → K-M3 → K-M2a → K-M2 → K-M5**

K-M4 moved ahead of K-M3 and was built on 2026-08-03. It was listed after
K-M2 because it reads "the HWM", but it only ever needed *a* sequence
position, and today's `next_id` is one — so it was the second unblocked
milestone rather than the fifth.

M1 first was non-negotiable and paid for itself: everything after it now
assumes what the engine does rather than believing it. **K-M3 and K-M4 move
ahead of K-M2** because they are genuinely independent and unblocked, while
K-M2 now sits behind K-M2a, which sits behind work in another document
(`docs/wal.md`). Ordering K-M2 second would have meant building an allocator
whose stated acceptance criterion it could not meet.

Two things M1 surfaced that belong to other documents and block claims made
in this one. Neither is this workplan's to fix; both are its to stop
asserting:

- **Object oids are re-issued on every boot** (`well_known.hpp`'s
  `kUserOidStart`), which falsifies the oid half of §1.2 with no crash
  involved. Owner: the catalog.
- ~~**The catalog holds ~62 columns across all user relations**~~ — **fixed
  2026-08-06.** The catalog relations chain now, exactly as user relations
  do: each fixed page id is a chain *root*, and a full page links to the
  next from a reserved range of low page ids. The ceiling moves from ~68
  column rows for the whole instance to ~7,800. Unrelated to id identity,
  recorded because K0 found it, and kept here struck rather than deleted so
  a reader of the original finding can see what became of it.

## 6. Out of scope

- Cabin and any other consumer feature's use of the invariant — their
  own specs cite this document.
- Re-key implementation (K-M6).
- Cross-relation or global id spaces; the id remains per-relation.
- Any *density* guarantee: K3 forbids relying on gap-freeness. Ordering
  is a different matter — this document does not promise it, but §1
  lists four subsystems that already depend on it, so removing it is its
  own decision rather than a licence K3 hands out.
- Persisting the object-oid counter (§1.2), and the catalog's
  single-page relation limit. Both surfaced in K-M1, both belong to the
  catalog.
