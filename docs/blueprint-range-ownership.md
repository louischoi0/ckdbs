# Blueprint — Range-Granular Core Ownership

**Proposal only; nothing here is implemented and nothing here is ratified.**
This is the end-state architecture blueprint for "dynamically allocated to
cores, reorganised on statistics, every core equivalent" — the revision the
operator opened 2026-08-24. It fixes the *shape* (what the ownership unit
is, what routes, what migrates) and leaves every constant, policy and
protocol choice `[OPEN]` with its owner named. Drafted in the main checkout
on `main` at `a755521`; every claim about existing code names its site.

Upstream of everything in it: `docs/spec-page-lsn-cross-stream.md` (the
PL decision). No phase that moves a page between streams may be built
before PL is ratified.

---

## 1. The ownership unit is the primary-key range

Not the relation, not the page.

- **The relation is too coarse.** It caps a hot relation at one core
  permanently, and no placement policy fixes that. For the engine's stated
  workload — financial OLTP — one dominant relation is the ordinary case.
- **The page is too fine.** `workplan-crosscore.md` M1 rejected page/extent
  hashing because "btree descent and heap-chain walks cannot cross cores
  per hop", and that argument survives any amount of mechanism: arbitrary
  per-page ownership makes nearly every descent hop a boundary. The cost is
  structural, not budgetary.
- **The pk range is the unit the engine already orders itself by.** Both
  clustering modes are range-ordered structures today: the heap chain is
  key-ordered page by page — "one page's ids are entirely below the next
  page's `min_key`" (`include/kds/storage/heap/heap_chain.hpp:38`) — and
  the clustered B+ tree is key-ordered by construction. `min_key` is
  immutable (invariant 2) so a range boundary is stable for the life of a
  page. A descent under range ownership crosses **at most one boundary, at
  the top**, and everything below it is core-local.

A **range** is `[lo, hi)` over the 40-bit Keystone id space of one
relation. A relation starts life as one range owned by its creating core —
which makes today's `sys.tables.owner_core` the degenerate case, not a
retired concept.

## 2. Why this fits *this* engine — the thesis argument

The project's two native mechanisms become the routing layer without
amendment:

- **Waystone** names pages; a page names a range; a range names a core.
  Invariant 9 already permits exactly this — Waystone chooses *where to
  look*, never what is visible, and "which core" is where-to-look. A trail
  replayed on the wrong core after a migration misses on the epoch/owner
  check and falls through, which is the ordinary miss discipline.
- **Cabin** is value-observed and authoritative for observed values
  (`docs/feat-cabin.md`), so it answers "which range holds value V" for a
  non-pk predicate without a broadcast, after first observation.
- A secondary-index entry is `key || pk || covered`
  (`include/kds/storage/index/index_tree.hpp:39`), so **a probe's answer
  names its own destination**: the pk it returns is the routing key.

Engine-driven physical reorganisation on runtime statistics is the
project's first thesis; range ownership is the same thesis on the
core axis, served by the same structures. That is the argument for
carrying the cost — not generic scalability.

## 3. What already exists and is load-bearing

| Existing piece | Role here | Site |
|---|---|---|
| Key-ordered chains / trees | ranges need no new physical order | `heap_chain.hpp:38`, invariants 2, 3, 11 |
| CC7 flush-then-grant handoff | the migration primitive, re-triggered | `docs/crosscore.md` CC7, workplan P6b |
| `relayout_epoch` + `owner_oid` in the common header | advisory invalidation and page attribution after a move | `docs/page.md` §2, §2a; `exec/tuple_verify.hpp` |
| Row-id block leases (P5-shape) | the insert-spreading mechanism (§6) | `catalog::RowIdLeaseTable`, `catalog.hpp:229` |
| Extent leases | allocation stays core-local per range | `storage/extent_lease.hpp` |
| Step pipeline + coroutines (P4a-P4e) | cross-range statements execute as today's cross-relation ones | `docs/workplan-crosscore.md` P4 |
| KWP row codec | one row format for every forwarded row | `wire/row_codec.hpp` |
| Trx-id lease (PW1) | ids global with no core bits | `server/trx_id_lease_service.hpp` |

## 4. The range directory

Ownership stays a **function of the catalog** — guideline 4 is kept, not
amended. A new catalog relation (working name `sys.ranges`: rel oid, lo,
owner core; hi is the next row's lo) records every split; a relation with
no rows there is one range owned by `sys.tables.owner_core`.

- **Resolution happens where it happens today**: the session core resolves
  owners at plan time from its catalog cache; a remote stage trusts its
  descriptor and does not re-resolve; staleness surfaces as a retryable
  step error (`crosscore.md` §5's existing rule, unchanged).
- **Cache invalidation is the one part that must be built carefully**: the
  known-gaps entry stands — `InvalidateFromPeer()` clears without bumping
  `catalog_version()`, so the prerequisite is the cache-generation counter
  every invalidation path bumps (`docs/known-gaps.md`, named 2026-08-15).
  A split/migration broadcast rides `kCatalogInvalidate` as DDL does.
- **The directory is read-mostly.** Splits and migrations are rare,
  DDL-frequency events; the per-statement path reads a per-core cached
  copy. CC1's fast-path invariant binds: a one-range relation on its owner
  core must add zero instructions over today.

## 5. Reads, writes, transactions

- **Reads**: a statement whose ranges all live on the session core runs the
  local fast path. Any other read is the step pipeline over ranges instead
  of relations — same messages, same credits, same teardown-by-tag. A scan
  spanning k ranges opens k stages; `emit_in_key_order` concatenates in
  range order, which range ordering makes structurally free.
- **Writes**: a single-range DML statement ships whole to the range's owner
  (`crosscore.md` §6 statement shipping — "involves no pipeline").
  CC3's home-core rule keeps holding *per stream*: a transaction's writes
  bind to one core. **A statement or transaction writing two ranges of one
  relation is therefore a cross-core write and stays refused retryably
  until 2PC** — range ownership widens what CC3 refuses, and this is
  stated rather than hidden. The refusal counters (`core_affinity.hpp`)
  are the evidence 2PC's design will be made from, exactly as §6 planned.
- **Visibility**: CC4 unchanged per range — a shipped stage reads the
  owning core's latest committed view; RR weakens per core. The cross-core
  commit oracle DT9 waits on is the same oracle multi-range transactions
  wait on; one design serves both.

## 6. The tail problem — the honest constraint, and the answer built in

**In `ASSIGNED` mode ids ascend, so every INSERT targets the relation's
maximum id — the tail range.** Naive range ownership spreads reads and
updates and leaves *inserts* single-core, which for insert-heavy OLTP
concedes the headline number.

The answer is already in the tree: **row-id block leases**
(`catalog.hpp:229` — with a table installed, `AllocateRowId()` draws from
the leased block; core 0 carves blocks). Let each core insert from its own
leased id block, and align ranges to block boundaries: every core then
appends to **its own range's tail**, fully locally. The heap chain's
refusal of an id below the tail's `min_key` (`heap_chain.hpp:129`,
invariant 3) is satisfied *per range* because each range is its own chain
tail. Consequences, stated now:

- Per-relation id monotonicity becomes per-range monotonicity. Invariant
  11 was already amended once (2026-08-11, §4.1: "monotonicity is now
  per-relation, never engine-wide"); this is the same amendment one level
  down, and it needs the same loud documentation.
- `EXPLICIT`-mode relations spread naturally (the caller's ids need not
  ascend) and need none of this.
- Whether interleaved blocks are the default or opt-in is `[OPEN]` —
  a single-writer relation gains nothing from them.

## 7. Migration, split, merge

- **Trigger**: the statistics substrate (§8). Split when one range's load
  dominates its core; migrate when cores imbalance; merge is `[OPEN]` and
  probably v2 (cold ranges cost only directory rows).
- **Mechanism**: CC7 generalised — flush, then the handoff the PL decision
  ratifies (PL-B logged handoff + PL-C stream stamp is the reading on
  record; **not ratified**), then grant, then the directory row, then the
  invalidation broadcast. `relayout_epoch` bumps on migrated pages so
  every Waystone/Cabin reference self-heals through the existing miss path.
- **Split point**: a page boundary, always — `min_key` is the split key,
  so no page is ever divided and invariants 2/3 hold by construction.
- **The mover is the physical optimizer's third part.** Part I moves
  tuples between pages (shadow-only, §6-gated); Part II manages Cabins;
  this is Part III, moving ranges between cores, and it inherits Part I's
  discipline: observe, decide, report through SHOW, enact only through
  named gates.

## 8. Every core equivalent — retiring M5

Required, and separable from ranges:

- Superblock, free map and catalog gain a partition-boundary lock each
  (rules.md §3's last-resort clause, justification in the subsystem
  header) *or* stay message-serialised through a rotating coordinator —
  `[OPEN]`, decided by measurement.
- DDL runs on any core; the peer DDL refusal (PW4) becomes unnecessary
  rather than unbuilt.
- Per-core listeners (PW5) stop being "peers forward to core 0" and start
  being the front door.
- Statistics relations become per-core (`crosscore.md` §2 already calls
  for it): a peer that records nothing cannot feed the mover, so this is a
  prerequisite of §7, not an optimisation.

## 9. Buffer pool

Global **frame accounting** first (one budget arbiter over the N private
pools — also fixes the live defect that `buffer_pool_frames` reaches core 0
only, `expeditor.cpp:599`). The frame *directory* — which core holds which
page — falls out of the range directory instead of being tracked per page:
a page's range names its owner, and only the owner faults it. The private
per-core pool structure survives unchanged.

## 10. What this blueprint deliberately gives up

- **Deterministic simulation pays a permanent tax.** Directory mutations
  and boundary locks are new interleaving points; each must be a seeded
  scheduling point or sim fidelity drops. Budgeted, not avoidable.
- **Multi-range transactions wait for 2PC.** Stated in §5; the blueprint
  widens CC3's refusal before it removes it.
- **Recovery gains a phase.** Handoff records (PL-B) must be analysed
  before redo scope is decided; mount cost grows with migration count
  since the last checkpoint.

## 11. Phasing — each stage shippable, none assuming the next

| Stage | Content | Gate |
|---|---|---|
| R0 | Ratify PL (`docs/spec-page-lsn-cross-stream.md`) | operator decision |
| R1 | Every core equivalent: shared-structure access rule, per-core listeners, per-core statistics relations | PL not needed |
| R2 | Global frame accounting | none |
| R3 | Range directory + read path: `sys.ranges`, manual `SPLIT RANGE` DDL, pipeline over ranges. Placement still static | R1 |
| R4 | Writes: single-range statement shipping; id-block-aligned insert spreading (§6) | R3, PW1b |
| R5 | The mover (physical optimizer Part III): statistics-driven split/migrate | R0, R1, R3; PL built |
| R6 | Multi-range transactions | 2PC — separate decision |

R1+R2 stand on their own merits even if ranges are never built.

## 12. Open decisions — do not assume

PL (owner: `wal.md` §15); per-range local vs global secondary indexes
(reading on record: local per range, broadcast probes cut by Cabin/Waystone
— **not ratified**; owner: `feat-index.md` §13); split/migrate policy and
its constants (owner: this doc once promoted); id-block interleave default
(§6); shared-structure access mechanism (§8); merge; 2PC.
