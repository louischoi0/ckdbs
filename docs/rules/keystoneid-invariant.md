# Keystone id — issue-once invariant

Status: **DECIDED** (K1–K5 below). `docs/rules/keystoneid-k0-findings.md`
is the audit's findings record. `docs/spec/heap-and-tuple.md` §4.1 owns
where an id comes from — the `INSERT` names it or omits it, per row, and
there is no key mode — so every "at or above the mark" / "below the mark,
refused" phrase here is that section's rule.
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
  (hard rejection at compile, no slow path).
- **K3 — No density promise, and order only within one relation.** Gaps
  are legal and expected (bump-ahead recovery, aborted inserts, an aborted
  sorted fill's carve); nothing may rely on ids being contiguous. Ids do
  ascend, per relation and in issue order: the cursor never goes
  backward, and a named key below the mark is refused on every relation
  (BB-R3, on BB-Q8 (b); `heap-and-tuple.md` §4.1), so every id a
  relation issues or admits is above every id before it, and no relation
  records an exception (`kUnordered` is deleted, BB-R10). Monotonicity is
  a **per-relation, per-history property, not an engine-wide one**: the
  ids of two relations, or of one relation across a re-key (K5), are not
  comparable.
- **K4 — Lifetime budget is a documented product constraint.** 2^40
  ids per relation is the relation's lifetime insert budget, stated
  openly in product docs rather than engineered around.
- **K5 — Offline re-key reserved.** The one sanctioned way to reset
  the budget is an explicit offline maintenance operation that
  consciously re-issues ids under an exclusive window. Reserved, not
  specified here (§4).

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
2. **(oid, pk) is a forever-unique key.** Every structure keyed on it —
   the statistics primitives, waystone trail entries, in-memory canonical
   caches, any replication or change feed — gets identity for free: a
   stored (oid, pk) can dangle, but it can never mis-attribute, making
   "dangling ⇒ skip" a universally sound rule. The oid half holds because
   `Catalog::GenerateUserOid()` recovers its position from the catalog on
   first use (`keystoneid-k0-findings.md` §6). The pk half — K1 across a
   crash — holds exactly as far as the mark's durability does:
   `sys.tables.next_id` advances through a logged catalog write that
   precedes the row it covers and replays with every other catalog write
   (`docs/spec/wal.md`), and the sorted fill's carve moves the mark the
   same way, so a crash burns ids and never reissues one.
3. **Audit posture.** For the finance-adjacent positioning: "a row's
   identifier never changes and is never reissued" is a compliance
   sentence, not just an implementation detail. Immutable, unique-for-
   all-time record identity is a precondition for defensible audit
   trails.
4. **Simpler invalidation everywhere.** Validation logic that would
   otherwise need epoch-style incarnation checks on ids reduces to
   existence + visibility checks.

What it deliberately does **not** promise (K3): gap-freeness, and any
order between the ids of two relations or across a re-key.

Four things rest on *ordering* rather than on uniqueness:

- the semi-sorted heap chain refuses an id below the tail page's
  `min_key` (`heap_chain.hpp`), which is invariant 3 enforced at the one
  place tuples enter — so it depends on *issuance* order, not only on
  values. It is fed a monotonic sequence whoever names the ids, because
  `Catalog::AdmitExplicitRowId` refuses a below-the-mark key on every
  relation (§2), and that refusal sits above `heap_chain.cpp`'s own.
- the clustered btree's pages hold their slots in key order at every core
  count, because a row's id is fixed under the exclusive hold of the leaf
  it lands on and is above every id placed before it (BB-R1, BB-R3) —
  the premise the `ORDER BY <pk>` elision, the Cabin serve's pk sort and
  the index step's (`index.md` IX8a) read. The leaf divide
  (`SplitLeafAndInsert`, `src/storage/btree/btree.cpp`) and the
  internal-node divide stay in `BtreeInsert`'s storage contract, which
  takes any id and whose leaf slot search assumes no order; SQL reaches
  neither.
- uniqueness comes from the mark, for an omitted key and for a named
  key, which is admitted only at or above it. A named key below it is
  refused, and on a btree the descent's scan of the one leaf it holds
  names a present one `AlreadyExists` rather than `OutOfRange` (BB-R12).
- `kRange`'s `min_key` tail pruning (`src/exec/step_vm.cpp`) rests on
  *page-wise* `min_key` ordering, which a leaf division preserves — the
  old leaf keeps its bound and the new one takes the split key. Value
  order across pages never required issuance order.

## 2. Allocator contract

Per relation, the allocator maintains a persisted **high-water mark**
(HWM) in `sys.tables.next_id`, and what the number means depends on the
arity of the `INSERT` (`heap-and-tuple.md` §4.1):

- for an **omitted key** it is *the smallest id never yet issued*, and it
  is both the source of the id and the proof it is unique;
- for a **named key** it is *a ceiling at or above every id placed so
  far*, and it gates on every relation (BB-R3): `id < next_id` is
  refused. It issues nothing. On a **heap** relation that comparison is
  the only uniqueness proof a chain has, and the refusal is `OutOfRange`
  whether the key is present or not; on a **btree** relation the descent
  that holds the key's leaf answers first, `AlreadyExists` for a present
  key and `OutOfRange` for an absent one (BB-R12).

The two readings share one monotone mark: an issued id clears every named
one, and a named one at or above the mark clears every issued one. Only a
*below-the-mark* named key could meet an issued id, and every relation
refuses one.

Rules:

- Issue = return current cursor, advance. The cursor never moves
  backward, and no free-list of any kind exists for Keystone ids.
  `Catalog::AllocateRowId` and `Catalog::AllocateRowIdRange` refuse
  nothing for a key reason.
- The mark is persisted **before** the row is placed, through the logged
  catalog write path (`docs/spec/wal.md`), so a crash in between leaves a
  ceiling that is too high — burning ids K3 calls free — never one too
  low.
- **Every core issues, and none caches** (AT-S10b). The mark is a
  `sys.tables` row bumped in place under its catalog page's latch, so two
  cores issuing into one relation are serialised by the latch and the ids
  stay one sequence in issue order (`heap-and-tuple.md` §4.1a). On a
  btree relation the bump runs under the exclusive hold of the rightmost
  leaf the row lands on (BB-R1), so placement order is issue order there
  too. Until AT-S10b a peer issued from a block core 0 carved and leased
  to it, which made the ids a sequence per core only.
- **Named keys — `Catalog::AdmitExplicitRowId(oid, id)`.** It first checks
  that the id is *spellable* — inside `[kFirstRowId, kMaxKeystoneId]`,
  else `InvalidArgument` — before the catalog page is touched. At or above
  the mark: `next_id = id + 1`, persisted before the row is placed, and on
  a btree relation under the exclusive hold of the rightmost leaf the key
  lands on (BB-R1). Below the mark, on every relation: refused
  (`RefuseRowIdBelowMark`, `OutOfRange`) with nothing written (BB-R3); a
  btree's descent refuses a present key `AlreadyExists` before this is
  asked (BB-R12). The mark's advance outlives a rollback, deliberately
  (`heap-and-tuple.md` §4.1). The mark issues nothing on this path, so a
  too-low mark cannot reissue an id here; it would admit a named key
  below one already placed, which is why persisting before placing keeps
  the gate sound as well as the ceiling truthful for K4.

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

- It is an **offline, exclusive** operation on one relation: a
  maintenance task with no concurrent statement on any core. The
  scheduling model provided that exclusivity while every statement on a
  relation ran on its owning core; since AT-S5 it does not, and what
  excludes other cores' statements is not specified here (the relation
  `X` of `docs/spec/txn.md` §5 is the lock family's candidate).
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

## 5. Milestones

The labels are what source comments and tests cite; each entry is what
stands in the tree under it.

**K-M1 — Audit of every issuance path.**
`docs/rules/keystoneid-k0-findings.md` and `tests/keystone_id_test.cpp`:
every path that issues an id, every path that could re-issue one, and the
exposure a durable log has to a sequence persisted outside it.

**K-M2 — Bump-ahead allocation.**
The block-size floor is **4096**, measured against the `bench/` tree at
`1769487` (`git show 1769487:bench/keystone_alloc_bench.cpp`): below it a
durable bump stops amortizing — one fsync per 64 rows is still one fsync
every 64 rows, a 3× INSERT regression at N=64 — and per-id durability caps
INSERT at the device's fsync rate. `txn::kTrxIdBlockSize` reuses the number
rather than re-deciding it, for the one sequence whose ceiling is synced
per block (the superblock's, `txn.md` §4.2); it is frozen like
`kds.inline_cell_width`, not per-relation tunable. **Row ids take no block
since AT-S10b**: every core's `AllocateRowId` bumps the mark per issued id
as core 0's always did, a logged catalog write with no sync of its own, and
the per-core row-id lease that had this shape (`kRowIdLeasePerGrant`,
4096) is gone - a per-core block breaks issue order across cores
(`heap-and-tuple.md` §4.1a).

**K-M2a — The ceiling is durable.**
`sys.tables.next_id`'s bump is a logged catalog write with an `UNDO_WRITE`
ahead of it and replays with every other catalog write
(`docs/spec/wal.md`); that is what lets K1 be called held across a crash
(§1.2).

**K-M3 — K2 enforced.**
`exec::CompileAssignments` (`src/exec/step_compiler.cpp`), called from
`UpdateInner` before any storage is touched — beside `CompileWhere`,
because those are the two halves of an UPDATE's compile, and a check the
dispatcher owns is one a second write path can be written without —
refuses an UPDATE whose SET list touches the pk. Four rules it carries:

- **The code is `kUnsupported`, and the split from `kInvalidArgument` is
  the point.** An unknown SET target is simply wrong; the primary key is
  *understood and declined* — the column exists and the value would
  encode, and what cannot happen is the write, because the id names the
  tuple in the clustered tree, in every index and Cabin entry, and in
  every recorded trail. It is the invariant, not a missing feature.
- **Both refusals carry a byte.** `parser::Assignment` has a
  `byte_offset` for the reason `AstValue::byte_offset` has one; nothing
  compares the field, and the fingerprint folds from the token stream and
  not from the AST, so no stored `pattern_id` depends on it.
- **The parser does not refuse it.** Which column is the pk is catalog
  knowledge; a parser that guessed from the name `id` would refuse a legal
  statement on a relation whose *second* column is called that. The
  parser-level test is a **negative** one — the statement parses — and the
  refusal is the compiler's.
- **Case sensitivity is a finding, not a fix.** `Schema::FindColumn`
  matches a SET target exactly, while the step compiler resolves a WHERE
  column through `IEquals`, so `SET ID = 99` against a pk named `id` is
  refused as an *unknown column* rather than as the pk. K2 holds either
  way — no path reaches the write. Making SET targets case-insensitive is
  a change to the engine's identifier rule (`manual/sql/sql.md`'s
  "statements are case-insensitive") and belongs to whoever owns it.

**K-M4 — Budget observability.**
Two surfaces: **`SHOW BUDGET`** lists every relation under a summary line
carrying `warning=<n>`/`exhausted=<n>`, so consumption is visible without
reading every row; and **`DESCRIBE`** reports `ids_issued`,
`ids_remaining` and `budget_used` beside the `next_id` they derive from
(`docs/spec/client-manual.md`).

The arithmetic is `catalog::BudgetOf()`
(`include/kds/catalog/keystone_budget.hpp`), a pure function of one
integer rather than a line of `<<` in the dispatcher, so the *source* can
change and none of the arithmetic moves. Three things it settles that an
inline subtraction gets wrong: **issued counts ids spent, not rows
living** (a burned id is spent, and a renderer saying "rows" would be
lying); capacity is `kMaxKeystoneId − kFirstRowId + 1`, one short of 2^40
because id 0 is reserved; and exhaustion is a flag rather than the tail of
a rounded percentage, since `AllocateRowId` refuses rather than wrapping.

The warning threshold is `kKeystoneBudgetWarnFraction` = 90%
`[PROPOSED]`: the input for a number is how long a relation takes to cross
the last 10% at its own insert rate (§3's table), which is
per-deployment. A named constant, so moving it is one edit.

**K-M5 / K-M6 — Documentation, and re-key.**
Nothing beyond this file stands under either label: §1's sentence and
§3's table are the text for product docs, and §4 fixes re-key's boundary
conditions without specifying it.

## 6. Out of scope

- Cabin and any other consumer feature's use of the invariant — their
  own specs cite this document.
- Re-key implementation (K-M6).
- Cross-relation or global id spaces; the id remains per-relation.
- Any *density* guarantee: K3 forbids relying on gap-freeness. Ordering
  is promised within one relation's history only (K3); §1 lists what
  rests on it.
- The object-oid counter and the catalog's page ceiling — both the
  catalog's (`keystoneid-k0-findings.md` §5, §6).
