# Waystone — Concept & Technical Specification

**Status:** **Official specification.** Confirmed 2026-07-28. Supersedes the earlier Waystone draft in full; the earlier bounded-pool (65,536-entry) model is **superseded** (§10). Sections marked `[CONFIRMED]` are settled; `[OPEN]` items must not be assumed. Milestones and task breakdowns live in the companion work-instruction document `waystone-workplan.md`, not here.

**Naming:** *Waystone* — extends the engine's stone metaphor: the **Keystone** word is each tuple's structural identity; a *waystone* guides travelers without being the road, which is exactly this structure's advisory role.

---

## 1. Concept

Waystone is KDS's generalization of heap tuple reference patterns into a maintained per-relation structure, and the substrate for both of the engine's differentiating mechanisms:

- **Physical relayout:** per-tuple heat (decayed use counts, recency) tells the physical optimizer which tuples are hot and which pages to rebuild — within what the immutable `min_key` invariant permits.
- **The new index form:** unlike a value-ordered index, Waystone maps *observed access* to tuple locations directly. It is the seed of the auto-generated hint index (`(query_pattern_id, args)` → cross-table locations).

Waystone lives **outside the query executor** as a separate module. The executor only emits access events through a one-method observer seam; all interpretation, storage, and policy belong to Waystone. This separation makes the advisory contract (§3) structurally enforceable.

## 2. Scope Model — Full Coverage `[CONFIRMED]`

**Decision (2026-07-28): Waystone tracks every tuple of an enabled relation** (Option 1). The bounded-pool alternative (tracking a hot subset with admission/eviction) is rejected.

Recorded rationale:

- Per-tuple overhead is 32 bytes (§5). Against typical OLTP row sizes this is a single-digit-percent tax; acceptable.
- Full coverage **eliminates the admission/eviction problem entirely** — the policy machinery, its bottleneck risk, and the performance cliff for untracked tuples all disappear.
- O(1) arithmetic access by id becomes possible for *every* tuple, which is what the hint index and relayout statistics need to be dependable.

Coverage is **per relation**, controlled by a catalog flag (§7). "Every tuple" means every tuple of relations with Waystone enabled.

## 3. Consistency Model `[CONFIRMED]`

The "100% consistency" question is split into two facts with different guarantees:

- **Coverage (existence) — guaranteed 100%.** Every tuple of an enabled relation has a Waystone entry. The entry slot is determined arithmetically by the tuple's pk (§4), so it is initialized as part of the **insert path** with an O(1) write. Note: design-spec invariant 8 forbids meta pages on the normal *read* path only; participation in the write path is permitted and is hereby confirmed for entry initialization.
- **Location (page_id/slot) — advisory, never authoritative.** Guaranteeing exact location at all times would make Waystone a second authoritative index: synchronous double-writes on every UPDATE and relayout, WAL logging, and crash-recovery obligations — contradicting hard invariant 7 and erasing the design's advantage. Instead, location freshness is tracked by **epoch validation**: each heap page carries an epoch counter, bumped whenever tuples on it move (relayout, page rebuild); each Waystone entry records the epoch at observation. `entry.page_epoch == heap page epoch` ⇒ the location is trustworthy; mismatch ⇒ consumer falls back to the B+ tree. Misses therefore exist only in the short window after a page's tuples move, before re-observation.

Normative advisory rules (restating hard invariants 7 & 8; enforced by tests):

1. A consumer acting on a Waystone location must validate the target (Keystone id match + MVCC visibility) or rely on epoch trust, and must fall back to the B+ tree on any mismatch.
2. Dropping any event or the entire structure may cost performance but must never change query results.
3. The ingest path never blocks, throttles, or fails a query. Ring overflow policy is drop.
4. Waystone pages never appear on the normal read path.

## 4. Addressing — pk-Direct O(1) `[CONFIRMED]`

Under full coverage, **the pk itself is the entry address**; no separate handle exists.

```
entry_index = pk                        // zero-extended 40-bit Keystone id
logical_page = pk >> 8                  // 256 entries per page
slot         = pk & 0xFF
```

Consequence — **Keystone word amendment required (§10):** the `meta_handle:16` field loses its addressing purpose. The 16 bits are redefined as **reserved** in the Keystone layout (`id:40 | flags:8 | reserved:16`); repurposing (e.g. a hot-tier accelerator handle, extended transaction-slot reference) is `[OPEN]`. Until repurposed, writers must set the field to 0 and readers must ignore it.

Space model: Waystone size is proportional to **issued ids**, not live tuples (32 B × highest issued pk, sparsely allocated per §6). High-churn relations that burn ids accumulate dead entries; id-reuse or low-range reclamation policy is `[OPEN]`. This is one reason for the operational guidance in §8.

## 5. Entry Format — 256 Bits `[CONFIRMED]`

One entry per tuple, exactly **256 bits (32 bytes)**, in ordinary 8 KB pages obtained through `storage::PageStore`.

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 8 | `pk` | zero-extended Keystone id (upper 24 bits 0); self-identifying for integrity checks. `rel_oid` is implicit — the structure is per-relation. |
| 8 | 4 | `page_id` | last observed heap location; advisory; `kInvalidPageId` if never observed |
| 12 | 2 | `slot` | last observed slot |
| 14 | 2 | `flags` | `kEntryLive` (set at insert, cleared on delete), rest reserved |
| 16 | 4 | `use_count` | decayed access counter |
| 20 | 4 | `last_ts` | truncated logical timestamp of last access |
| 24 | 4 | `page_epoch` | heap-page epoch at observation (§3); location trusted iff it matches the page's current epoch |
| 28 | 4 | `reserved` | future: pattern link / argument affinity |

Derivations (named `constexpr`s in code): `kEntrySize = 32` (power of two ⇒ shift/mask addressing); `kEntriesPerPage = 8192 / 32 = 256`, tiling the page exactly.

Format rules (rules.md §2, §5): field-wise `memcpy` codec only; no `reinterpret_cast` overlays; no compiler bitfields; fixed-width integers; little-endian on disk; sizes/offsets pinned by `static_assert`.

Semantics: **heat and location are separate facts.** An epoch bump invalidates location trust; `use_count` survives — the tuple is still hot, we merely no longer know where it is until re-observed.

## 6. Logical Continuity — Per-Relation Page Directory `[CONFIRMED]`

Waystone pages cannot be physically contiguous — random inserts make that impossible. Continuity is **logical**, via a per-relation page directory (the inode-block-map pattern):

- The relation's catalog entry gains a **Waystone directory root `PageId`** (plus the enable flag, §7).
- A directory page (8 KB) holds `8192 / 4 = 2048` child `PageId`s. Coverage per level: 1 level → 2048 × 256 = 524,288 tuples; 2 levels → 2048² × 256 ≈ 1.07 × 10⁹; **3 levels** → 2048³ × 256 = 2⁴¹, covering the full 40-bit id space with headroom. Depth is fixed at what the relation's id high-water requires; growing depth relinks the root.
- Lookup: `pk` → digits base-2048 → directory walk → leaf Waystone page → `slot = pk & 0xFF`. Constant hops (≤ 4 page touches); upper directory levels are few and stay core-resident, so the practical cost is the two-touch path.
- **Lazy allocation:** unpopulated ranges hold `kInvalidPageId` at every level; pages are allocated on first entry initialization. Sparse id spaces cost only what they touch.
- Directory and entry pages are ordinary pages from `PageStore`; ownership is core-local per the relation's owning core (rules.md §3).

## 7. Per-Relation Enablement — Catalog Flag `[CONFIRMED]`

Waystone is switchable **per relation**. The catalog stores a `waystone_enabled` flag on the relation's entry, and **the engine consults this flag to decide the relation's storage form**:

- **Enabled at creation:** the directory root is provisioned; the insert path initializes entries (coverage guarantee active); the executor's observer wiring includes the relation; probes are served.
- **Disabled:** no directory, no entry pages, no per-insert work, no events aggregated for the relation. The relation stores data exactly as a plain semi-sorted heap.
- **Disabling a live relation** is always safe (advisory contract): drop the directory and entry pages wholesale, clear the flag. No query result changes.
- **Enabling a live relation** requires a coverage **backfill**: a `maintenance`-group task scans the relation (B+ tree order), initializes entries, and only then sets the flag's *coverage-complete* state. Until backfill completes, probes answer NotFound and the coverage guarantee is not yet claimed. Backfill is restartable and, like everything here, droppable.
- Flag changes are catalog DDL; their durability follows catalog persistence rules.

## 8. Operational Guidance — Recommendation to Operators `[CONFIRMED, non-normative]`

*This section addresses database operators and customers, not engine developers. It is a recommendation, not an engine-enforced restriction.*

**Enable Waystone on master (reference) tables; leave it off elsewhere by default.** Master data — customers, accounts, instruments, counterparties — is where Waystone pays: read-heavy PK point lookups, comparatively low churn, bounded and long-lived id populations, and recurring query shapes that the hint index can learn. High-churn transactional tables (order flow, tick/history, audit logs) are a poor fit: they burn ids (space grows with issued ids, §4), their access is append/scan-shaped rather than repeated point access, and their statistics decay before they can be exploited.

Rule of thumb: if a table is something you would cache, enable Waystone on it. If it is something you archive, do not.

## 9. Runtime Model (summary)

Unchanged from the confirmed event/aggregator design, restated post-decision:

- **Two-layer events:** query-template fingerprint (`pattern_id`, computed once at parse/plan time) + 48-byte fixed POD `TupleAccessEvent` per tuple touch (now carrying no meaningful `meta_handle`; field retired per §10).
- **Observer seam:** `AccessObserver::OnTupleAccess(...) noexcept` is the executor's only knowledge of Waystone — wait-free enqueue into a core-local SPSC ring, or drop. `NullAccessObserver` remains a valid production configuration and is the implicit configuration for waystone-disabled relations.
- **Aggregator (write side):** core-local, `maintenance` scheduling group, never self-throttling (SLO controller is the only throttle). `Ingest` updates count/recency/location/epoch; `DecayTick` applies decay against the injected clock; epoch bumps from relayout invalidate location trust in place of the old page-sweep invalidation. Admission machinery is **deleted** — there is nothing to admit.
- **Probe (read side):** O(1) by pk through the directory; returns location + `page_epoch` + heat; consumers apply §3 rule 1. For hint consumers and the physical optimizer only.
- **Insert/delete hooks:** insert initializes the entry (`kEntryLive`, coverage); delete clears `kEntryLive`. Both are O(1) arithmetic writes.
- **Derived patterns:** range access `(rel_oid, pk_lo, pk_hi)` and pattern-correlated groups (per-execution `SingleKeyRef` sequences keyed by `(pattern_id, arg_hash)`) are aggregations over the same primitive and event stream.

## 10. Superseded Designs & Required Amendments `[CONFIRMED, normative]`

This decision amends previously confirmed design. The following changes are **mandatory documentation work**; the concept is not fully confirmed until they land (task breakdown in `waystone-workplan.md`):

1. **Design spec, Keystone column section:** layout becomes `id:40 | flags:8 | reserved:16`. The `meta_handle` field and its "temporary metadata pool identifier" semantics are removed; the 16 bits are reserved (write 0 / ignore), repurposing `[OPEN]`. Record the amendment date.
2. **Design spec, metadata pool section:** the per-relation 65,536-entry bounded pool, its 16-bit handle addressing, its eviction management, and the 2 MiB cap are **superseded** by Waystone full coverage (this document). Keep the section with a superseded-by note rather than deleting history.
3. **Design spec, heap page section:** add the per-page **epoch counter** (storage location `[OPEN]` — header field vs core-local table; if a header field, it is on-disk format and follows rules.md §5).
4. **Catalog spec / `well_known`:** relation entries gain `waystone_enabled` (+ coverage-complete state) and the directory root `PageId`.
5. **CLAUDE.md:** remove the now-obsolete open decisions (metadata pool eviction policy, eviction-vs-validation invalidation); add the new `[OPEN]` items from §11; update the architecture summary (Keystone layout, metadata pool → Waystone).
6. **`docs/page-management.md`:** no structural change, but note Waystone directory/entry pages as a `PageStore` client and a future SpaceManager consumer.
7. **Header `waystone.hpp`:** rewrite to match — delete `AdmissionPolicy` and 16-bit-handle addressing; add directory constants/walk, epoch field, insert/delete hooks, catalog-flag wiring.
8. **Earlier Waystone draft document:** replaced by this specification + `waystone-workplan.md`.

## 11. Open Decisions — do not assume

- Repurposing of the freed 16 Keystone bits (reserved until decided).
- Heap-page epoch storage (page header on-disk field vs core-local epoch table) and epoch width/wraparound handling.
- Id-reuse / low-range reclamation policy for high-churn enabled relations.
- Waystone page persistence class (WAL-logged vs unlogged; on unlogged loss, rebuild = backfill).
- Decay function and cadence (halving vs EWMA; tick period).
- Ring sampling policy under pressure (drop-newest vs drop-oldest vs probabilistic).
- Directory depth growth protocol details (root relink ordering vs concurrent probes on the owning core).
- Hint-index admission and per-template trust classification (unchanged from before).

## 12. Testing Requirements

All deterministic (injected clock, simulated scheduling; rules.md §4):

1. **Codec & directory:** entry round-trips; offset/size asserts; directory walk correctness incl. lazy allocation and depth growth; page tiling exact.
2. **Advisory contract:** query results byte-identical with observer off, ring saturated (all drops), and after wholesale Waystone deletion of an enabled relation.
3. **Read-path isolation:** instrumented `PageStore` proves zero Waystone-page touches during normal query execution.
4. **Coverage guarantee:** after any committed insert on an enabled relation, the entry exists with `kEntryLive`; after delete, cleared. Backfill converges and is restartable mid-way.
5. **Epoch validation:** relayout bumps epoch ⇒ probes report untrusted; re-observation restores trust; counts survive throughout.
6. **Flag semantics:** disabled relations incur zero Waystone work (instrumented); enable→backfill→coverage-complete transition; disable drops structures with no result change.
7. **Overflow:** ring saturation drops without blocking; drops visible in `stats()`.
