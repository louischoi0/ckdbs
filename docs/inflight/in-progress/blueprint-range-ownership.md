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
shared-structure `[OPEN]` the btree's top-of-tree hop lands on. One
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
| R0 | ~~Ratify PL~~ — **closed 2026-08-24**, PL-B + PL-C guard (`docs/spec/page-lsn-cross-stream.md` §9) | done |
| R1 | Every core equivalent: shared-structure access rule, per-core listeners, per-core statistics relations | PL not needed |
| R2 | Global frame accounting — **static half built 2026-08-24** (the instance budget divides over every core per EV4, worktree `r2-frame-budget`); the dynamic arbiter that rebalances shares by demand remains | none |
| R3 | Range directory + read path: `sys.ranges`, manual `SPLIT RANGE` DDL, pipeline over ranges. Placement still static | R1 |
| R4 | Writes: single-range statement shipping; id-block-aligned insert spreading (`crosscore.md` §6b, per-range chains included) | R3, PW1b |
| R5 | The mover (physical optimizer Part III): statistics-driven split/migrate | R1, R3; the PL contract built |
| R6 | Multi-range transactions | 2PC — separate decision |

R1+R2 stand on their own merits even if ranges are never built.

## 12. Open decisions — do not assume

Per-range local vs global secondary indexes
(reading on record: local per range, broadcast probes cut by Cabin/Waystone
— **not ratified**; owner: `index.md` §13); split/migrate policy and
its constants (promoted 2026-08-24: `crosscore.md` §9 indexes it, the
physical optimizer's Part III spec owns it when drafted); id-block
interleave default (§6; indexed at `crosscore.md` §9); shared-structure
access mechanism (§8); merge; 2PC. The split *gates* — which relations
may split at all before these decisions land — are ratified rules, not
open: `crosscore.md` §6a.
