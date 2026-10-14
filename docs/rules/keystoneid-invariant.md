# Keystone id — issue-once invariant

Status: **DECIDED** (K1–K5 below). `docs/rules/keystoneid-k0-findings.md`
is the audit's findings record. `docs/spec/heap-and-tuple.md` §4.1 owns
where an id comes from — the `INSERT` names it or omits it, per row, and
there is no key mode — so every "at or above the mark" / "below the mark"
phrase here is that section's rule. BD
(`instructions/v3.0.0/workorder-bd-sorted-leaf-named-keys.md`) restated K1
and K3 on 2026-10-07; BH (`instructions/v3.0.0/workorder-bh-purge-key.md`)
gave K1 its one named exception, `PURGE`, on 2026-10-08
(`heap-and-tuple.md` §4.1c); BK (`instructions/v3.0.0/workorder-bk-keystone-48.md`)
widened the id to 48 bits on 2026-10-11 (K4, §3).
Depends on: Keystone super-column contract (48-bit id + 8-bit flags +
8-bit reserved), per-relation catalog metadata, WAL.
This document deliberately covers the engine-level id contract only;
features that *consume* the invariant keep their own specs.

Decisions fixed here:

- **K1 — Issue-once, with one named exception.** A Keystone id is bound
  to at most one committed tuple in the lifetime of a relation, **except
  across a `PURGE` that freed it**. No other built path rebinds it: not the
  allocator, not delete-then-insert, not crash recovery, and not a purge
  the engine runs on its own. **An id is consumed when a tuple carrying it
  commits** (BD-R4): a committed key stays in its leaf, live and then
  delete-marked, and the delete-marked row is its tombstone - until a
  `PURGE` retires it. An id issued and burned without placement, or placed
  and rolled back, was never bound, and a named key may take it.
  - **`PURGE` is the operator's statement** (BH-R1). It frees the deleted
    keys of a tuple key window, each whole and in key order, visibly and logged,
    and only once no reader can see the row each deleted (`heap-and-tuple.md` §4.1c, PU5). After it,
    an `INSERT` naming the key is judged as if the key had never been
    placed. The issue cursor never hands a purged key out (K3): only an
    `INSERT` that names it takes it again.
  - **An engine-internal purge still owes a keyed, never-visible
    tombstone in the row's key position** - which the duplicate check and
    the placement order read and every walk treats as absent - or it
    reclaims nothing.
- **K2 — Immutable.** A tuple's Keystone id never changes after
  insert. An UPDATE that targets the super column is **Unsupported**
  (hard rejection at compile, no slow path).
- **K3 — No density promise, and order only for issued keys within one
  relation.** Gaps are legal and expected (bump-ahead recovery, aborted
  inserts, an aborted sorted fill's carve); nothing may rely on ids being
  contiguous. **Issued keys are a sequence in issue order**: the cursor
  never goes backward. **Named keys carry no order** on a btree, which
  places one wherever it sorts (BD-R7); a heap admits one only at or above
  the mark (`heap-and-tuple.md` §4.1, kept by BD-Q4 (a)). A btree leaf's
  slot order is its key order by placement, and no relation records an
  exception (`kUnordered` is deleted, BB-R10). "Uniqueness needs no page
  read" holds on a heap only; on a btree an issued id is unique among
  issued ids by the mark and against named keys by the placement's
  duplicate check. Monotonicity is a **per-relation,
  per-history property, not an engine-wide one**: the ids of two
  relations, or of one relation across a re-key (K5), are not comparable.
- **K4 — Lifetime budget is a documented product constraint.** 2^48
  ids per relation is the relation's lifetime insert budget, stated
  openly in product docs rather than engineered around.
- **K5 — Offline re-key reserved.** The one sanctioned way to reset
  the budget is an explicit offline maintenance operation that
  consciously re-issues ids under an exclusive window. Reserved, not
  specified here (§4).

---

## 1. The invariant and why it earns its place

> **Keystone ids are issued once, bound to at most one committed tuple,
> never rebound except by an explicit `PURGE`, never mutated.**

What this buys, engine-wide:

1. **Snapshot-safe tuple key resolution.** Without issue-once, a tuple key freed and
   re-issued while an old snapshot can still see the prior tuple makes
   plain tuple key lookups resolve to the wrong incarnation — the reader
   walks the *new* tuple's undo chain and silently misses a row its
   snapshot is entitled to. Issue-once deletes the hazard structurally
   instead of gating it behind a purge-horizon rule that every future
   feature would have to re-prove. **`PURGE` is that rule, built for one
   statement** (BH-R5): it retires a tombstone only once every live and
   future reader sees its deleter as committed, and every view that can
   read a superseded user row is registered with that horizon (BH-S1's
   Census A) - an obligation every later view kind inherits. A rolled-back
   key named again does not reopen it: the rolled-back tuple was never visible to another
   snapshot, the new row starts no undo chain, and every structure that
   keeps `(oid, tuple key)` past a write verifies against the row now there
   (`workorder-bd-sorted-leaf-named-keys.md` §1.7).
2. **(oid, tuple key) is unique at any instant, and unique for life up to a
   `PURGE`.** Every structure keyed on it — the statistics primitives,
   waystone trail entries, in-memory canonical caches, any replication or
   change feed — gets identity for free: a stored (oid, tuple key) can dangle;
   a committed tuple key names another row only after a `PURGE` freed it, and one
   a rolled-back insert left can be named again - both of which the
   read-time verification against the row now there catches (BH-S1's
   Census B checked every consumer) - so "dangling ⇒ skip" is sound. The oid half holds because
   `Catalog::GenerateUserOid()` recovers its position from the catalog on
   first use (`keystoneid-k0-findings.md` §6). The tuple key half — K1 across a
   crash — holds exactly as far as the mark's durability does:
   `sys.tables.next_id` advances through a logged catalog write that
   precedes the row it covers and replays with every other catalog write
   (`docs/spec/wal.md`), and the sorted fill's carve moves the mark the
   same way, so a crash burns ids and never reissues one.
3. **Audit posture.** "A row's identifier never changes and is never
   reissued **except by an explicit, logged `PURGE`**" is a compliance
   sentence, not just an implementation detail. An external system that
   captured a Keystone id must treat a purged key as a new identity, and
   the purge is in the log to say when it became one.
4. **Simpler invalidation everywhere.** Validation logic that would
   otherwise need epoch-style incarnation checks on ids reduces to
   existence + visibility checks.

What it deliberately does **not** promise (K3): gap-freeness, any order
among named keys, and any order between the ids of two relations or
across a re-key.

Four things rest on *ordering* rather than on uniqueness:

- the semi-sorted heap chain refuses an id below the tail page's
  `min_key` (`heap_chain.hpp`), which is invariant 3 enforced at the one
  place tuples enter — so it depends on *issuance* order, not only on
  values. It is fed a monotonic sequence whoever names the ids, because
  `Catalog::AdmitExplicitRowId` refuses a below-the-mark key on a heap
  (§2). For a named key on a heap the chain's own check runs first, under
  the tail's hold, and the mark's inside it (BB-R7,
  `heap::ChainInsertNamed`); for an omitted key the id is issued there.
- the clustered btree's leaves hold their keyed slots in key order at
  every core count, because a row takes the slot its key sorts to
  (BD-R1, BD-R2) — the premise the `ORDER BY <tuple key>` elision, the Cabin
  serve's tuple key sort and the index step's (`index.md` IX8a) read. It rests
  on placement, not on issue order, so a named key below the relation's
  highest is placed, and reaches the leaf divide and the internal-node
  divide from SQL.
- uniqueness comes from the mark for an omitted key. For a named key it
  comes from the mark on a heap, which admits one only at or above it,
  and from the descent on a btree: a slot keyed `k` in the one leaf that
  may hold it, live or delete-marked, is `AlreadyExists` (BD-R5).
- `kRange`'s `min_key` tail pruning (`src/exec/step_vm.cpp`) rests on
  *page-wise* `min_key` ordering, which a leaf division preserves — the
  old leaf keeps its bound and the new one takes the split key. Value
  order across pages never required issuance order.

## 2. Allocator contract

Per relation, the allocator maintains a persisted **high-water mark**
(HWM) in `sys.tables.next_id`, and what the number means depends on the
arity of the `INSERT` (`heap-and-tuple.md` §4.1):

- for an **omitted key** it is *the smallest id never yet issued*, and it
  is the source of the id and proves it unique among issued ids - on a
  btree a named key equal to an id drawn but not yet placed can be placed
  first, and the placement's duplicate check (`SearchLeaf`) then finds it
  and the issue draws again, bounded at `kMaxIssueRounds`; on a heap the
  mark is the whole proof;
- for a **named key** it is *a ceiling at or above every id placed so
  far*. It issues nothing. On a **heap** relation it gates: `id <
  next_id` is refused `OutOfRange` whether the key is present or not,
  that comparison being the only uniqueness proof a chain has (BB-R12,
  kept by BD-Q4 (a)). On a **btree** relation it gates nothing (BD-R7):
  a key at or above it moves it, one below leaves it, and the descent
  answers uniqueness.

The two readings share one monotone mark: an issued id clears every named
key admitted before its draw, and a named one at or above the mark clears
every issued one. A
*below-the-mark* named key can meet an id issued earlier on a btree, and
the descent answers it as a duplicate when that id is bound; a heap
refuses it. A named key equal to an issued id not yet placed can borrow
it and place it first, and the issue then draws again.

Rules:

- Issue = return current cursor, advance. The cursor never moves
  backward, and no free-list of any kind exists for Keystone ids.
  `Catalog::AllocateRowId` and `Catalog::AllocateRowIdRange` refuse
  nothing for a key reason.
- The mark is persisted **before** the row is placed, through the logged
  catalog write path (`docs/spec/wal.md`), so a crash in between leaves a
  ceiling that is too high — burning ids K3 calls free — never one too
  low.
- **Every core issues, and none caches.** The mark is a
  `sys.tables` row bumped in place under its catalog page's latch, so two
  cores issuing into one relation are serialised by the latch and the ids
  stay one sequence in issue order (`heap-and-tuple.md` §4.1a). On a heap
  the bump runs under the exclusive hold of the chain's tail (BB-R7), so
  placement order is issue order there too; on a btree it runs under no
  page hold since BD (BD-R6), and an id issued before a higher one placed
  first lands below it, where it sorts.
- **Named keys — `Catalog::AdmitExplicitRowId(oid, id)`.** It first checks
  that the id is *spellable* — inside `[kFirstRowId, kMaxKeystoneId]`,
  else `OutOfRange`, the exhausted reason (BD-R5) — before the catalog
  page is touched. At or above the mark: `next_id = id + 1`, persisted
  before the row is placed - on a btree before the descent, under no page
  hold (BD-R6), on a heap under the tail's hold (BB-R7). Below the mark:
  on a btree nothing moves and nothing is refused (advance or nothing,
  BD-R7); on a heap refused (`RefuseRowIdBelowMark`, `OutOfRange`) with
  nothing written. The mark's advance outlives a rollback, deliberately
  (`heap-and-tuple.md` §4.1); on a btree the rolled-back key is free
  regardless, its slot retired keyless. The mark issues nothing on this
  path, so a too-low mark cannot reissue an id here; on a heap it would
  admit a named key below one already placed, which is why persisting
  before placing keeps the gate sound as well as the ceiling truthful for
  K4.

## 3. Lifetime budget (K4) — the honest math

Issue-once converts 2^48 (≈ 2.81 × 10^14) from a live-row bound into a
**lifetime issuance budget per relation**, consumed by every insert,
including rolled-back ones and bump-ahead gaps.

| sustained insert rate (one relation) | budget exhausted in |
|---|---|
| 1,000 /s | ~8,900 years |
| 5,000 /s | ~1,800 years |
| 50,000 /s | ~180 years |
| 500,000 /s | ~18 years |
| 5,000,000 /s | ~1.8 years |

Product-doc stance: for master/account-class relations the budget is
effectively unlimited; for the highest-rate ingest relations (trade/event
logs sustaining hundreds of thousands of inserts per second for years) it
is reachable and must be planned for. The sanctioned patterns, in order:

1. **Relation partitioning by period** (monthly/quarterly log
   relations) — already standard OLTP operational practice; each
   partition gets its own 2^48.
2. **Offline re-key** (K5) as the escape hatch when partitioning was
   not applied in time.

Widening the id beyond 40 bits was considered and rejected before BK, on
the ground that it would forfeit the fixed 64-bit super-column word
(40+8+16) the tuple header, meta-pool handle and page arithmetic were
built on. **Overturned on 2026-10-11 by the operator** (BK, W2,
`instructions/v3.0.0/raft-marks-2026-10-11-keystone-48.md` §1): the word stays 64
bits (48+8+8), the meta-pool handle had already been retired, and nothing
in page arithmetic read the id's width (`workorder-bk-keystone-48.md`
§1.7), so the reason no longer held and the id took eight bits of the
reserved field. K5 stays reserved (BK-Q2 (a)).

## 4. Offline re-key (K5) — reserved semantics

Not specified in this document; the reservation fixes only its
boundary conditions so nothing else accidentally forecloses it:

- It is an **offline, exclusive** operation on one relation: a
  maintenance task with no concurrent statement on any core. What
  excludes other cores' statements is not specified here (the relation
  `X` of `docs/spec/txn.md` §5 is the lock family's candidate).
- It deliberately violates K1 **once, atomically, and visibly**:
  every tuple receives a fresh id from a reset HWM; the operation is
  logged as a single recoverable unit.
- Everything keyed on the old (oid, tuple key) space is invalidated
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
The block-size floor is **4096**: below it a durable bump stops
amortizing (N=64 was a 3x INSERT regression) and per-id durability caps
INSERT at the device's fsync rate (measured pre-v3 with
`bench/keystone_alloc_bench.cpp` at `1769487`).
`txn::kTrxIdBlockSize` reuses the number for the one sequence whose
ceiling is synced per block (the superblock's, `txn.md` §4.2); it is
frozen, not per-relation tunable. **Row ids take no block**: every core's
`AllocateRowId` bumps the mark per issued id, a logged catalog write with
no sync of its own - a per-core block would break issue order across
cores (`heap-and-tuple.md` §4.1a).

**K-M2a — The ceiling is durable.**
`sys.tables.next_id`'s bump is a logged `HEAP_OVERWRITE` of its catalog
row at `kNoTxnId` (`OverwriteLogged`, `src/catalog/catalog.cpp`) - no
undo, because no transaction owns it and a bump is never rolled back - and
replays with every other catalog write (`docs/spec/wal.md`); that is what
lets K1 be called held across a crash (§1.2).

**K-M3 — K2 enforced.**
`exec::CompileAssignments` (`src/exec/step_compiler.cpp`), called from
`UpdateInner` before any storage is touched, refuses an UPDATE whose SET
list names the tuple key:

- **`kUnsupported`, not `kInvalidArgument`**: an unknown SET target is
  simply wrong; the tuple key is understood and declined, because the id names
  the tuple in the clustered tree, every index and Cabin entry, and every
  trail. Both refusals carry the byte (`parser::Assignment::byte_offset`,
  which nothing compares and the fingerprint does not fold).
- **The parser does not refuse it**: which column is the tuple key is catalog
  knowledge, so the parser-level test is a negative one.
- **Case sensitivity is a known finding**: `Schema::FindColumn` matches a
  SET target exactly while WHERE resolves through `IEquals`, so `SET ID =
  99` against a tuple key named `id` is refused as an *unknown column*. K2 holds
  either way. Column names are unique ignoring case (`catalog.md` CT7), so
  `ID` can name no other column of a relation created since that rule.

**K-M4 — Budget observability.**
**`SHOW BUDGET`** lists every relation under a summary line carrying
`warning=<n>`/`exhausted=<n>`; **`DESCRIBE`** reports `ids_issued`,
`ids_remaining` and `budget_used` beside `next_id`
(`docs/spec/client-manual.md`). The arithmetic is `catalog::BudgetOf()`
(`include/kds/catalog/keystone_budget.hpp`), a pure function of
`next_id`: issued counts ids spent, not rows living; capacity is
`kMaxKeystoneId − kFirstRowId + 1` (id 0 is reserved); exhaustion is a
flag, since `AllocateRowId` refuses rather than wrapping. The warning
threshold `kKeystoneBudgetWarnFraction` = 90% is `[PROPOSED]`: the right
number is per-deployment (how long the last 10% takes at §3's rates).

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
