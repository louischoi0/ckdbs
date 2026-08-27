# Keystone id — issue-once invariant (concept + workplan)

Status: **DECIDED** (K1–K5 below). **K-M1 done 2026-08-03** —
`docs/rules/keystoneid-k0-findings.md` is what it found, and its four proposed
amendments are **applied here** (2026-08-03): K3's wording, §1's min_key
aside, §5's milestone order, and §1.2's oid claim. Read the findings before
starting K-M2; three of them change what K-M2 is.

**Amended again 2026-08-25 by `docs/spec/heap-and-tuple.md` §4.1**, which
**removed the key mode**. K1, K2, K4 and K5 are untouched a second time. What
moves is the scoping every "per mode" phrase below rests on: there is no
mode, `AllocateRowId` and `AllocateRowIdRange` refuse nothing for a key
reason, and `AdmitExplicitRowId` runs on every relation. **The reserved rule
§2 records as deleted came back, scoped to the heap** — `id < next_id` is
`OutOfRange` on a heap-clustered relation — because there the mark is the
only uniqueness proof available and §3.1b's chain rests on it. Read every
`ASSIGNED`/`EXPLICIT` below as *"a key at or above the mark"* / *"a key below
it, btree-only"*; the arguments survive the renaming, and where they do not,
the 2026-08-25 notes say so.

**Amended 2026-08-11 by `docs/spec/heap-and-tuple.md` §4.1** (the `EXPLICIT` key
mode, **built** the same day). K1, K2, K4 and K5 are untouched. **K3 and §2
are not**: the amendment removed monotonicity for explicit relations, which
K3 had reserved as a separate decision, and it did **not** ship the
explicit-id gate §2 had written for it. Both are corrected below, in place,
with the difference stated rather than smoothed over — §2's rule and the
shipped rule are not the same rule, and a reader who quotes the old one will
be wrong about what the engine accepts.

**Milestone state (corrected 2026-08-10 — this line read "K-M2..K-M6 not
started", which §5 below already contradicted for K-M4):**
**K-M1 done**, **K-M4 done** (both 2026-08-03), **K-M3 done 2026-08-10**.
**K-M2a, K-M2, K-M5, K-M6 not started**, and K-M2 stays blocked on K-M2a,
which is blocked on work in `docs/spec/wal.md`.
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
- **K3 — No density promise, and since 2026-08-11 no ordering promise
  either.** Gaps are legal and expected (bump-ahead recovery, aborted
  inserts); nothing may rely on ids being contiguous. Issue-once does
  not *by itself* promise ordering, and this clause used to close with
  "removing monotonicity is a separate decision with its own blast
  radius, not a consequence of this one."
  **That decision has now been made**, by `docs/spec/heap-and-tuple.md`
  §4.1's `EXPLICIT` key mode: on such a relation the caller names the
  id and **it need not ascend**. So K3's subject widens twice over —
  ids may be sparse *and* out of order — and the blast radius was paid
  where it lands rather than avoided. It landed entirely inside the
  clustered btree: a full leaf now divides, a full internal node divides
  too, and the leaf slot search no longer assumes key order. Nothing outside the btree
  changed, because nothing outside it depended on issuance order —
  see the corrected list in §1.
  **What did not move:** the cursor still never goes backward, and the
  semi-sorted heap chain still sees a monotonic sequence — **by the mark
  rather than by the mode since 2026-08-25**, when `AdmitExplicitRowId`
  took over the refusal `CREATE TABLE` used to make. Monotonicity is a
  **per-relation, per-history property, not an engine-wide one** — code
  that needs it must read `key_order`, never assume it, and never derive
  it from the storage type either: a btree relation fed only ascending
  keys is as monotonic as any heap.
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

It does not promise **ordering** either. The engine used to have it
anyway, from §2's allocator, and four things depended on it. **Corrected
2026-08-11**, when the `EXPLICIT` key mode dropped the ascent for
btree-clustered relations and each of the four had to be settled:

- the semi-sorted heap chain refuses an id below the tail page's
  `min_key` (`heap_chain.hpp`), which is invariant 3 enforced at the one
  place tuples enter — so it depends on *issuance* order, not only on
  values. **Kept, by scoping**: an `EXPLICIT` relation must be
  btree-clustered, so every heap chain is still fed a monotonic
  sequence. This is the dependency that decided the shape of the whole
  feature. **Re-scoped 2026-08-25** and kept again, more cheaply: the
  scope moved from the relation to the id, `AdmitExplicitRowId` refusing
  a below-the-mark key on a heap relation, so the chain is fed a
  monotonic sequence whoever named the ids. `heap_chain.cpp` did not
  change — this refusal sits above the one it names.
- the clustered btree used to **refuse a non-monotonic id outright**,
  with `OutOfSpace` naming the open split-policy decision rather than
  guessing — the strongest of the four. **Paid off**: `SplitLeafAndInsert`
  divides a full leaf instead (`src/storage/btree/btree.cpp`,
  `heap-and-tuple.md` §4.1). One refusal of this class survives, and it is
  narrower still, and then closed: a separator promoted into a *full
  internal node* below that node's highest divides its entries (PK09),
  which leaves no refusal of this class at all.
- `keystone.hpp` derives uniqueness *from* monotonicity ("unique and
  monotonic by construction rather than by a uniqueness check").
  **Replaced, per mode**: on `ASSIGNED` that derivation stands; on
  `EXPLICIT` uniqueness comes from the descent, which scans the one leaf
  that may hold the key and answers `AlreadyExists`.
- `kRange`'s `min_key` tail pruning stops a walk on the strength of it
  (`src/exec/step_vm.cpp`). **Untouched**: it rests on *page-wise*
  `min_key` ordering, which a leaf division preserves — the old leaf keeps
  its bound and the new one takes the split key. Value order across pages
  never required issuance order.

None of that is a promise this document makes. All of it was a promise
something had to re-make before monotonicity could be dropped, and this is
the record of which ones were re-made and how.

## 2. Allocator contract

Per relation, the allocator maintains a persisted **high-water mark**
(HWM) in `sys.tables.next_id`. **What that number means is now
mode-dependent** (`heap-and-tuple.md` §4.1), and conflating the two
readings is the mistake this section previously made:

- for an **omitted key** it is *the smallest id never yet issued*, and it
  is both the source of the id and the proof it is unique;
- for a **named key** it is *a ceiling at or above every id placed so
  far*. It issues nothing. On a **heap** relation it still gates — `id <
  next_id` is `OutOfRange`, and that comparison is the only uniqueness
  proof a chain has. On a **btree** relation it gates nothing; the descent
  does, and the mark exists only so K4's budget and the 40-bit exhaustion
  check stay truthful about the id space consumed.

**The two readings meet on every relation since 2026-08-25**, where the
2026-08-11 shape kept them apart by refusing whole relations. What keeps
them from colliding is that they share one monotone mark: an issued id
clears every named one, and a named one at or above the mark clears every
issued one. Only a *below-the-mark* named key can meet an issued id, which
is why only a btree relation admits one and why the descent is what answers
there.

Rules:

- Issue = return current cursor, advance. The cursor never moves
  backward, and no free-list of any kind exists for Keystone ids.
  `Catalog::AllocateRowId` and `Catalog::AllocateRowIdRange` refuse
  nothing for a key reason — the 2026-08-11 `Unsupported` on a
  `kExplicit` relation is gone with the mode.
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
- **Explicit-id inserts — corrected 2026-08-11, and this bullet used to
  be wrong about what shipped.** It reserved a gate: *an explicit id ≥
  HWM advances the HWM past it; an explicit id < HWM is rejected, since
  it may collide with an issued id and proving otherwise would require
  the free-list this design forbids.* When `heap-and-tuple.md` §4.1
  built the `EXPLICIT` key mode it did **not** spend that clause. It
  deleted the second half.

  **What `Catalog::AdmitExplicitRowId` actually does:** it checks that
  the id is *spellable* — inside `[kFirstRowId, kMaxKeystoneId]`, else
  `InvalidArgument` — and that the relation is `kExplicit`, else
  `Unsupported`. Then it advances the mark with `max()`: at or above,
  `next_id = id + 1`, persisted before the row is placed; **below, it
  returns having written nothing**. There is no ordering check and no
  `OutOfRange`.

  **Corrected 2026-08-25: half the reserved rule came back.** The mode
  check is gone (there is no mode), and below the mark the function now
  splits on storage: a **heap** relation gets exactly the `OutOfRange`
  this section records as deleted, and a **btree** relation is admitted as
  described, plus a once-ever `key_order` flip. So the paragraphs below
  are right about the btree and wrong about the engine — the reserved
  rule was not too strong in general, it was too strong *for a relation
  with a descent*. Where the chain has no descent, its cheapness is
  exactly what makes caller-named keys possible at all.

  **Why the reserved rule had to go.** It rested on "HWM is the smallest
  id never issued, so `id ≥ HWM` proves non-collision with no page
  read". True — but it is a proof about ids *the allocator* issued, and
  on a relation where the caller issues them the mark stops being
  evidence: nothing says the ids below it were used. Enforcing it would
  refuse correct inserts (a backfill of older keys) while proving
  nothing about the ones it admitted, since a caller can name an id
  above the mark that a *previous* caller already placed above the mark
  in the same way. So **uniqueness moved to the descent**: `BtreeInsert`
  scans the one leaf the descent lands on and answers `AlreadyExists`.
  That is a page read the insert was making anyway, and it is why the
  mode is restricted to btree-clustered relations — a heap chain has no
  such walk, and the reserved rule's cheapness was the only thing that
  would have made one unnecessary. **Which is what 2026-08-25 then did**:
  it kept the reserved rule on the heap instead of keeping the relation
  off caller-named keys, and the sentence above turns out to have been
  the design.

  **What the mark still owes.** Because it never falls below an id
  placed while the process was up, K4's budget and the 40-bit
  exhaustion check stay honest, and an `EXPLICIT` relation cannot later
  be read as if its ids were dense. Persisting *before* the row is
  placed is kept for `AllocateRowId`'s reason and with a weaker
  consequence: a crash in between leaves a ceiling that is too high,
  which burns space K3 calls free, where the reverse leaves one too low.
  A too-low mark on an explicit relation cannot reissue anything — the
  mark issues nothing — so the K1 break this ordering exists to prevent
  is an `ASSIGNED`-only hazard. When bump-ahead (K-M2) lands, the
  explicit path needs the same treatment for the same reason it needs it
  now: only to keep the ceiling truthful, never to keep an admission
  decision sound.

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

Delivered as `docs/rules/keystoneid-k0-findings.md`, `tests/keystone_id_test.cpp`
and `bench/results-keystone-alloc.md`. Headline: **K1 does not hold across a
crash**, because the durable log names ids that the unlogged `next_id` has
forgotten — a durability problem, not an allocator one, which K-M2 cannot
close alone. The demonstrating tests are green rather than red on purpose;
each names the condition under which it must be inverted, on the grounds
that a permanently-red test is one that gets ignored.

**K-M2 — HWM + bump-ahead allocator. BLOCKED on K-M2a.**
Implement §2: persisted per-relation HWM, chunked bump-ahead, recovery
resume-from-ceiling. Deterministic tests: crash
between chunk persist and issuance (sim-crash via IoBackend seam)
must never re-issue; gaps appear and are harmless.

Acceptance, restated after K-M1 — the original wording claimed a crash
property this milestone cannot deliver on its own, at any chunk size:

- the allocator issues from an in-memory interval and touches the
  catalog row once per chunk rather than once per id;
- a restart resumes from the persisted ceiling, never below it, and the
  skipped remainder appears as a gap;
- ~~an explicit id below the HWM is rejected and one at or above it
  advances the HWM past it~~ — **dropped 2026-08-11.** Half of it never
  shipped: `AdmitExplicitRowId` admits any spellable id and advances the
  mark with `max()` (§2). What K-M2 owes the explicit path is only that
  the ceiling stay at or above every placed id, which is a budget
  obligation, not an admission one;
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
read it back. Both belong to `docs/spec/wal.md`; they are named here because
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

**K-M3 — Enforce K2 (immutability). DONE 2026-08-10.**
Compiler/executor: an UPDATE whose SET list touches the super column
returns Unsupported at compile time (J2 policy — no slow path).
Acceptance: negative tests at parser, compiler, and wire levels.

*Built as `exec::CompileAssignments`* (`src/exec/step_compiler.cpp`),
called from `UpdateInner` before any storage is touched. It sits beside
`CompileWhere` because those are the two halves of an UPDATE's compile,
and a check the dispatcher owns is one a second write path can be written
without.

Four things it settled, three of them beyond moving the existing check:

- **The code is `kUnsupported`, and the split from `kInvalidArgument` is
  the point.** An unknown SET target is simply wrong; the primary key is
  *understood and declined* — the column exists and the value would
  encode, and what cannot happen is the write, because the id names the
  tuple in the clustered tree, in every index and Cabin entry, and in
  every recorded trail. It is not a missing feature that a later release
  implements; it is the invariant.
- **Both refusals now carry a byte.** `parser::Assignment` gained a
  `byte_offset` for the reason `AstValue::byte_offset` has one
  (`types.md` TY05) — and for the same reason it was safe: nothing
  compares the field, the fingerprint folds from the token stream and not
  from the AST, so no stored `pattern_id` moved.
- **The parser deliberately does not refuse it.** Which column is the pk
  is catalog knowledge; a parser that guessed from the name `id` would
  refuse a legal statement on a relation whose *second* column is called
  that. So the parser-level acceptance test is a **negative** one — the
  statement parses — and the refusal is the compiler's.
- **Case sensitivity is left exactly as it was, and is a finding rather
  than a fix.** `Schema::FindColumn` matches a SET target exactly, while
  the step compiler resolves a WHERE column through `IEquals`. So
  `SET ID = 99` against a pk named `id` is refused as an *unknown column*
  rather than as the pk. K2 holds either way — no path reaches the write
  — which is why this was not smuggled in here: making SET targets
  case-insensitive is a change to the engine's identifier rule, it
  contradicts `manual/sql/sql.md`'s "statements are case-insensitive", and
  it belongs to whoever owns that rule.

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
which is where someone already looks. `docs/spec/client-manual.md` has both.

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

> **K-M1 (done) → K-M4 (done) → K-M3 (done) → K-M2a → K-M2 → K-M5**

K-M4 moved ahead of K-M3 and was built on 2026-08-03. It was listed after
K-M2 because it reads "the HWM", but it only ever needed *a* sequence
position, and today's `next_id` is one — so it was the second unblocked
milestone rather than the fifth.

M1 first was non-negotiable and paid for itself: everything after it now
assumes what the engine does rather than believing it. **K-M3 and K-M4 move
ahead of K-M2** because they are genuinely independent and unblocked, while
K-M2 now sits behind K-M2a, which sits behind work in another document
(`docs/spec/wal.md`). Ordering K-M2 second would have meant building an allocator
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
