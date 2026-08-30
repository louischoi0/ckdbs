# Blueprint — Range-Granular Core Ownership

**The shape is ratified; the phases are not built.** The ownership unit
and the rules over it were **promoted into `docs/spec/crosscore.md` on
2026-08-24** (operator-directed v2 revision, worktree
`v2-crosscore-range-rules`: CC8-CC10, §2a, §5, §6-§6b) — that spec owns
the rules, and §§1, 4-7 here are pointers into it, not statements of
their own. This file keeps the thesis argument (§2), the
existing-pieces table (§3), the every-core-equivalent and buffer-pool
halves (§§8-9), the trade-offs (§10), the phasing (§11) and the open
index (§12). Every constant, policy and protocol choice stays `[OPEN]`
with its owner named. This is the end-state architecture blueprint for
"dynamically allocated to cores, reorganised on statistics, every core
equivalent" — the revision the operator opened 2026-08-24. Drafted in
the main checkout on `main` at `a755521`.

Upstream of everything in it was `docs/spec/page-lsn-cross-stream.md`
(the PL decision) — **ratified 2026-08-24: PL-B logged handoff with the
PL-C stream stamp** (that doc's §9). R0 is closed; every phase that moves
a page between streams builds against that contract.

---

## 1. The ownership unit is the primary-key range

**Promoted: `docs/spec/crosscore.md` CC8** — the unit, the too-coarse /
too-fine argument, the per-range sub-structure qualification (a heap
range is its own chain, a btree range its own subtree entry), and the
`[OPEN]` the btree's top-of-tree hop lands on — which since 2026-08-30 is
that hop **alone**, §8's system-structure rule having been decided and
CC11 not reaching a structure core 0 does not write. One
line: a range is `[lo, hi)` over the 40-bit Keystone id space of one
relation; a relation starts life as one range owned by its creating
core, which makes `sys.tables.owner_core` the degenerate case, not a
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
  (`docs/spec/cabin.md`), so it answers "which range holds value V" for a
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
| CC7 flush-then-grant handoff | the migration primitive, re-triggered | `docs/spec/crosscore.md` CC7, workplan P6b |
| `relayout_epoch` + `owner_oid` in the common header | advisory invalidation and page attribution after a move | `docs/spec/page.md` §2, §2a; `exec/tuple_verify.hpp` |
| Row-id block leases (P5-shape) | the insert-spreading mechanism (§6) | `catalog::RowIdLeaseTable`, `catalog.hpp:229` |
| Extent leases | allocation stays core-local per range | `storage/extent_lease.hpp` |
| Step pipeline + coroutines (P4a-P4e) | cross-range statements execute as today's cross-relation ones | `docs/inflight/in-progress/workplan-crosscore.md` P4 |
| KWP row codec | one row format for every forwarded row | `wire/row_codec.hpp` |
| Trx-id lease (PW1) | ids global with no core bits | `server/trx_id_lease_service.hpp` |

## 4. The range directory

**Promoted: `docs/spec/crosscore.md` CC9 and §2a** — `sys.ranges` (with the
lo = 0 partition rule and the per-range entry page), plan-time
resolution, the read-mostly rule, and the cache-generation prerequisite
(`docs/inflight/known-gaps.md`, named 2026-08-15). Guideline 4 kept: ownership
stays a function of the catalog.

## 5. Reads, writes, transactions

**Promoted: `docs/spec/crosscore.md` §2/§2a (reads), §6 (writes), §5
(visibility).** Note the visibility half was corrected in promotion:
this section's original "CC4 unchanged per range" is **retracted** —
per-stage views can tear a transaction that writes two same-core
ranges (or two same-core relations, latent in the shipped shape), so
`crosscore.md` §5 adds the one-view-per-(statement, core) rule, gated
on the peer writer. The cross-core commit oracle DT9 waits on is still
the oracle multi-range transactions wait on; one design serves both.

## 6. The tail problem — the honest constraint, and the answer built in

**Promoted: `docs/spec/crosscore.md` §6b** — id-block-aligned insert
spreading over the row-id block leases, with the per-range-chain
qualification CC8 adds (the leases supply the ids; the per-range chains
supply the tails; R3/R4 builds the second). Invariant 11's amendment
one level down — per-range monotonicity — needs the same loud
documentation when built.

## 7. Migration, split, merge

**Promoted: `docs/spec/crosscore.md` CC10 and §6a** — the split point, the
six-step migration ordering (quiesce → flush → handoff record →
directory row → grant → broadcast, abort-to-outgoing at mount before
the grant), advisory-reference retirement priced rather than assumed
(Cabin does not self-heal and gates migration), and the split gates.
**Trigger** stays here with §8: split when one range's load dominates
its core; migrate when cores imbalance; merge is `[OPEN]` and probably
v2 (cold ranges cost only directory rows). The mover is the physical
optimizer's Part III and inherits Part I's discipline.

## 8. Every core equivalent — retiring M5 — **declined as written**

**Declined by the operator, 2026-08-31**
(`instructions/v2.7.0/r1-catalog-placement-ratification.md`, CR4).
M5 stands: **core 0 is the sole writer of the reserved range, and every
core reads it.** This section is kept as the record of what was proposed
and what became of each part, not as work; what replaces it is
`docs/spec/crosscore.md` **CC11** (the access rule) and **CC12** (catalog
page placement and DDL's route), which own the rules and their reasoning.

| §8 item, as written | Disposition |
|---|---|
| 1. Superblock, free map and catalog take a partition-boundary lock each, or stay message-serialised | **Moot** — one writer, no contention (**CC11**, operator decision 2026-08-30). The `[OPEN]` is *removed, not resolved*, and with it the unsettled question of whether `rules.md` §3's partition-boundary allowance is an exception to guideline 1 — no document answers it and this decision no longer needs it answered |
| 2. DDL runs on any core; PW4 becomes unnecessary | **Declined** (CR2, **CC12**). A peer ships the DDL to core 0 and waits, so `PeerDdlRefused` (`src/server/command_dispatcher.cpp:918`) **stays necessary** rather than becoming unnecessary. The item wanted the refusal to stop existing; the ruling keeps it and routes around it |
| 3. Per-core listeners become the front door | **Partial, by consequence.** Reads are already equal — `DevicePageStore::MayFault` admits the whole system range on a leased store, and its own comment calls that a correctness requirement (`src/storage/device_page_store.cpp:651-656`) — while writes ship to core 0. PW5 is built; this ratification opens no further work here |
| 4. Statistics relations become per-core | **Preserved as the requirement, declined as the mechanism.** What R5 needs is that a peer's accesses reach the statistics at all; **CR7** supplies that by *batching* — a peer accumulates locally and flushes to core 0, which applies the batch to the one `sys.access_stats` at page 11, each entry carrying its `core_id`. So per-core **relations** were not the route taken, and the questions they would have opened (oid allocation, migrating page 11's rows, what a core-count change does to a departed core's relation) do not arise. It is **R5's gate** (CB7), not R1's item — see §11 |

**One reading of the ruling is not yet confirmed and is flagged where it
matters.** The operator's fourth decision was written as "항목 8 기각";
§8 has four items and no item 8, so CLA read it as *§8 itself is
declined*, which is the reading CR1-CR3 are consistent with. If item **4**
was meant instead, **R5 does not open** — §6a of `crosscore.md` states
that a peer recording nothing cannot feed the mover. The CR carries the
question; this table is settled only once it is answered.

## 9. Buffer pool

Global **frame accounting** first (one budget arbiter over the N private
pools). The *static* half is built as of 2026-08-24: `buffer_pool_frames`
is an instance total divided evenly per core, remainder to core 0
(`docs/spec/eviction.md` §6 EV4), which retires the defect this section
used to name — the key reaching core 0's pool alone. What R2 still wants
is the **arbiter**: shares are fixed at boot, and no core may borrow a
frame from an idle peer. The frame *directory* — which core holds which
page — falls out of the range directory instead of being tracked per page:
a page's range names its owner, and only the owner faults it. The private
per-core pool structure survives unchanged.

## 10. What this blueprint deliberately gives up

- **Deterministic simulation pays a permanent tax.** Directory mutations
  are new interleaving points; each must be a seeded scheduling point or
  sim fidelity drops. Budgeted, not avoidable. **Halved 2026-08-30**: the
  boundary locks this bullet also counted are not built — §8's rule is one
  writer per system structure, so the system structures add no interleaving
  point at all, and only the directory's does.
- **Multi-range transactions wait for 2PC.** Stated in §5; the blueprint
  widens CC3's refusal before it removes it.
- **Recovery gains a phase.** Handoff records (PL-B) must be analysed
  before redo scope is decided; mount cost grows with migration count
  since the last checkpoint.

## 11. Phasing — each stage shippable, none assuming the next

| Stage | Content | Gate |
|---|---|---|
| R0 | ~~Ratify PL~~ — **closed 2026-08-24**, PL-B + PL-C guard (`docs/spec/page-lsn-cross-stream.md` §9) | done |
| R1 | ~~Every core equivalent~~ — **declined as a stage, 2026-08-31** (§8, `instructions/v2.7.0/r1-catalog-placement-ratification.md`). The three rulings that replace it are `crosscore.md` **CC11** (every core reads, core 0 alone writes — the store's existing check, nothing to build) and **CC12** (CR1: catalog roots stay reserved, their var-heaps do not; CR2: a peer **ships** DDL to core 0 and waits, so `PeerDdlRefused` **stays**; CR3: a grown catalog page may leave the reserved range on the ordinary relation rules). **Per-core listeners: built** (PW5). What was this row's remaining owed item — per-core statistics relations — is **not R1's any more**: it has a mechanism (CR3) and a consumer (R5), and it is listed as R5's gate below | closed — nothing buildable remains under this row |
| R2 | Global frame accounting — **static half built 2026-08-24** (the instance budget divides over every core per EV4, worktree `r2-frame-budget`); the dynamic arbiter that rebalances shares by demand remains | none |
| R3 | Range directory + read path: `sys.ranges`, engine-internal range allocation behind the §6a gates — no user-facing range DDL, in this phase or any later one (operator direction 2026-08-27) — pipeline over ranges. Placement still static | R1 |
| R4 | Writes: single-range statement shipping; id-block-aligned insert spreading (`crosscore.md` §6b, per-range chains included) | R3, PW1b |
| R5 | The mover (physical optimizer Part III): statistics-driven split/migrate | **R3; peer-recorded access statistics (CB7)**; the PL contract built. **Restated 2026-08-31**: the gate used to read "R1, R3", and after CR4 declined R1 as a stage that named nothing buildable. What the mover actually needs from that stage is that a peer's accesses reach `sys.access_stats` at all — a peer that records nothing cannot feed it (`crosscore.md` §6a). CR7 supplies that **without** a per-core relation: `sys.access_stats` stays one relation at page 11, core-0-written, and a peer batches locally and flushes to core 0 with its `core_id` on each entry (order `instructions/v2.7.0/cb-catalog-placement-buildout.md`, CB7). CB satisfies this one prerequisite and **does not open R5** — split/migrate policy and its constants stay `[OPEN]` |
| R6 | Multi-range transactions | ~~2PC — separate decision~~ — **the gate is satisfied**: cross-owner transactions are built and specified (`docs/spec/cross-owner-txn.md`, 2026-08-28), so R6 inherits the protocol rather than designing one. Remaining gate: **R3**, for RD3's resolver |

R2 stands on its own merits even if ranges are never built. **R1 no longer
does** — not because it failed but because it was declined as a stage on
2026-08-31 (§8): its access rule is the store's existing check (CC11), its
listeners are built (PW5), and its one remaining buildable item became
R5's gate (CC12/CR3).

**What R6 inherits, recorded here so the next reader does not re-derive
whether 2PC exists** (`instructions/v2.5.0/cross-owner-protocol-closing.md`'s
closing clause). The commit protocol is complete **at relation
granularity** and R6 reuses it **unchanged**: a participant is a *core*,
never a relation — enrolment is keyed on `(coordinator core, session_id)`
and nothing in prepare, decide, resolution or recovery names a relation at
all. So the only row that changes under ranges is **owner discovery**,
which today reads `sys.tables.owner_core` and under ranges reads RD3's
resolver. Everything else — the two phases, the decision living in one
stream, the join rule, the in-doubt ceiling, the per-participant watermark,
the mount-time resolution — arrives already built and already measured
(`bench/v2.5.0/results-rr-read-half-*.md`).

Two costs R6 inherits with it, both named rather than discovered later: a
participant that only **read** is still prepared with a durable record and
a full decide (7-30x the read that enrolled it), and a read shipped inside
a cross-owner transaction is bounded by the one-slot reply cap. Both are
`crosscore.md` §9's, not R6's.

## 12. Open decisions — do not assume

**Core-count change: narrowed 2026-08-28 by operator direction.** The
count may change **in both directions**, and the reorganisation is a
**mount-time operation**, in the window RV1 establishes — after the
superblock is read, before the listener binds. **Online change is not
supported and is not a goal**: a scope decision, not an architectural
exclusion, and it forecloses nothing an online path would later need,
since revocation and quiesce would layer onto the same reassignment
logic rather than replace it (at mount there is no fault grant to
revoke — the store is built fresh). Three constraints ride with it and
are not negotiable: prepared transactions **resolve before** anything is
reorganised, and an unresolved prepare refuses the mount, because
reassigning a coordinator's stream destroys the evidence R6-4 resolves
against; the superblock's `core_count` is written **last**, so a crash
mid-reorganisation reads as the old count and the work reruns —
reassignment is therefore **idempotent**; and **modulo is not required**,
because placement policy is this blueprint's mover's (§7) and
correctness needs only that relations whose owner core no longer exists
move. *When* is settled; *how* stays open here and in `wal.md` §3 and
`superblock.hpp`'s pin.


Per-range local vs global secondary indexes
(reading on record: local per range, broadcast probes cut by Cabin/Waystone
— **not ratified**; owner: `index.md` §13); split/migrate policy and
its constants (promoted 2026-08-24: `crosscore.md` §9 indexes it, the
physical optimizer's Part III spec owns it when drafted);
~~shared-structure access mechanism (§8)~~ — **closed 2026-08-30**, every
core reads and core 0 alone writes (`crosscore.md` CC11), with the btree's
top-of-tree hop left open under its own name in `crosscore.md` §9;
merge; 2PC. The id-block
interleave default closed 2026-08-27 as **default** (CLA's reading of
the operator's range direction, correctable; `crosscore.md` §6b
carries it). The split *gates* — which relations may split at all
before these decisions land — are ratified rules, not open:
`crosscore.md` §6a.
