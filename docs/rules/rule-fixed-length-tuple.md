# Fixed-Length Tuples & the Var-Heap — Technical Specification

**Status:** **Official specification**, decisions confirmed 2026-08-01. This document graduates a discussion draft (`quarry/fixed-length-tuples-discussion.md`) into normative design; per the quarry rule the draft was to remain as the argument record — **but it is not in this repository** (noted 2026-08-10), so this file is both the argument record and what implementers follow. The rationale retained inline below is therefore load-bearing rather than a summary of something else. Rationale is retained inline — every decision here carries its *why*. Markers: `[CONFIRMED]`, `[PROPOSED]`, `[OPEN]`. Consistent with `docs/rules/rules.md`, `docs/spec/heap-and-tuple.md`, `docs/spec/wal.md`, `docs/spec/txn.md`, `docs/spec/waystone-concpets.md`, and `docs/spec/physical-optimizer.md` (the physical-optimizer spec — written 2026-08-09, backfilling the blueprint this line cited before one existed).

## 0. Decision Record `[CONFIRMED 2026-08-01]`

| # | Decision | Choice |
|---|---|---|
| V0 | The rule | **Every tuple is fixed-length.** Variable-width types occupy fixed-size tagged cells; oversize values live out of line in the **var-heap** |
| V1 | Inline threshold | **Global configuration constant** (`kds.inline_cell_width`), pinned per instance at bootstrap — not a per-column declaration |
| V2 | Var-heap shape | **Immutable per version**: updates write a new value and swap the pointer; var-heap bytes are never rewritten or moved in place |
| V3 | Var-heap durability | **Logged, headered, checksummed** — an ordinary authoritative page class (`kVarHeap`), not an advisory one |
| V4 | Prefix-inline for spilled values | **No** (revisit trigger recorded in §9) |
| V5 | Schema evolution | No width syntax exists (consequence of V1); changing the instance constant on existing data is **`Unsupported`** |

## 1. Background & Rationale

Two confirmed positions make tuple mobility a first-order concern in KDS. Relayout is the product — the physical optimizer's value compounds with anything that makes moving tuples cheaper or unintended movement rarer. And trails bet on location stability — every Waystone entry is a recorded `(page_id, slot, epoch)`, so the trail hit rate is a direct function of how often tuples sit still.

Variable-length rows attack exactly this: an UPDATE that grows a row can force it to move (Postgres pays as broken HOT chains, InnoDB as row migration), and in KDS the same event would additionally burn trail entries through epoch churn. Fixing the tuple length removes the disease at the root: **an UPDATE can never migrate a tuple**, and combined with the immutable `min_key`, a tuple's address is stable for life — until the physical optimizer moves it on purpose.

Secondary gains, each real: relayout becomes cell-`memcpy` with exact fill-factor math; in-page slot addressing becomes index arithmetic; the row codec reads static offsets (the engine's existing fixed-record discipline — Waystone entries, trail pages, frame headers — extended to user tuples); and the threshold at which relayout pays drops because moves got cheaper.

The honest cost, accepted knowingly: variable-length management is **relocated, not eliminated** — it moves into the var-heap (§5), and fixed cells spend space on padding for short values. The acceptance argument: the hot heap is where fixed length matters; the var-heap is deliberately boring (V2 makes it immovable); and on target OLTP schemas — short, uniform strings — a sane inline width keeps the common case entirely inline (§9 keeps the width's default value under measurement).

## 2. The Rule (normative)

- A relation's tuple layout is a sequence of fixed-size cells at offsets computable from the schema alone. Row size is a per-relation constant; in-page slot addressing is arithmetic.
- Fixed-width types (integers, floats, bool, timestamps, the Keystone word, MVCC header) occupy their natural widths as today.
- Every variable-width type (`TEXT`/`VARCHAR`, future blobs) occupies exactly **one tagged cell** of `kds.inline_cell_width` bytes (§3), regardless of the value stored.
- No code path may produce a tuple whose size differs from its relation's constant. This is asserted in the row codec, not policed by convention.

## 3. The Tagged Cell `[CONFIRMED format; widths PROPOSED]`

Cell width `W = kds.inline_cell_width`. Layout (memcpy codec, `static_assert`ed, LE — rules.md §2/§5):

| Tag (`u8` at offset 0) | Layout after tag | Meaning |
|---|---|---|
| `kNull` | zeros | SQL NULL; wire NULL convention maps 1:1 |
| `kInline` | `len u16`, then `len` bytes, zero padding | value fits: `len ≤ W − 3` |
| `kSpilled` | `len u32`, `varheap_ptr u64` (`page_id u32 · slot u16 · reserved u16`) | bytes live in the var-heap |

- The inline capacity is therefore `W − 3`; the spill decision is a pure function of value length — no heuristics, no per-row variance.
- An UPDATE that crosses the boundary in either direction changes the cell's *tag*, never the tuple's size.
- Rationale for a tag byte over sentinels: NULL, empty string, and spilled must be distinguishable without reading the var-heap, and the tag is where future cell kinds (V4 revisit) land without a format bump.

## 4. The Global Constant `[CONFIRMED semantics; default PROPOSED]`

`kds.inline_cell_width` is configuration-referenced but **instance-pinned**: read from configuration once at bootstrap, written into the superblock, and validated at every startup — a running configuration that disagrees with the superblock refuses to start (`InvalidArgument`, naming both values). It cannot be hot-changed; on-disk tuple layout depends on it, so changing it for existing data is a rebuild, which is `Unsupported` (V5).

Rationale for global-over-per-column (the decision that replaced the strawman): one number instead of a schema decision users can get wrong; one codec path instead of per-column widths threaded through every layout computation; no `VARCHAR(n)` grammar, no `ALTER … WIDEN` question — V5 falls out for free. The recorded cost: uniform padding overhead where a per-column width would have been tighter. Accepted as the simplicity trade.

Default: **64 bytes** `[PROPOSED]` — chosen so common OLTP strings (codes, names, references) never touch the var-heap; the honest counter-cost is 64 B per string column per row. The default stays `[PROPOSED]` until measured against real target-schema string-length distributions (§9); the *semantics* above are confirmed regardless of the number.

## 5. The Var-Heap `[CONFIRMED]`

The out-of-line value store. Its design goal is to be **boring**: the mobility problem was removed from the heap and must not reappear here.

- **Immutable per version (V2).** Writing a spilled value appends `{len, bytes}` to a var-heap page and returns its pointer. Values are never rewritten and never moved. Consequences, which are the rationale: MVCC correctness is free — an old-version reader follows the old pointer to bytes that cannot have changed; pointers need no epoch, no validation, no forwarding; the var-heap is **relayout-exempt by construction** (the physical optimizer never touches `kVarHeap` pages); reclamation is not new machinery but a rider on purge — when a version dies, its values die with it. The accepted cost: churn-heavy string updates consume space until purge catches up, making purge cadence a sizing input (§9 metric).
- **Logged and headered (V3).** `kVarHeap` joins the headered page-class enum: common header, `page_lsn`, CRC32C, full WAL participation via a `VARHEAP_APPEND` record. Rationale, stated because the recent reflex runs the other way: everything added lately (waystone/trail pages) was advisory, but a var-heap value is **authoritative data** — losing one loses a committed value, not a hint. The advisory rules do not apply and must not be pattern-matched onto this class.
- Write ordering on the update path: `VARHEAP_APPEND` (new value) → heap cell overwrite (`HEAP_OVERWRITE`, old cell image into undo) — both in the same transaction, replayed by the ordinary winner/loser machinery; a crash between them leaves an unreferenced value that purge's reclamation sweep collects. No special recovery logic may exist for the var-heap.

## 6. Interactions with Confirmed Design

- **min_key heap:** strengthened — tuple addresses now change only under deliberate relayout.
- **MVCC (`trx_id` + `undo_ptr`):** unchanged; the undo record's old-cell image is fixed-size like everything else, and under V2 it is just the old tag+bytes-or-pointer.
- **Trails/Waystone:** pure beneficiary — fewer epoch bumps, higher validated-hit rates; no format impact.
- **Physical optimizer:** moves get cheaper (cell memcpy) and `kVarHeap` is explicitly outside its jurisdiction.
- **Parser/DDL:** *simplified* by V1 — no width syntax is added; `TEXT` stays as-is.
- **Wire protocol:** invisible, and must remain so: `TEXT` on the wire is length-prefixed bytes regardless of inline/spilled storage.
- **Row codec/executor:** static-offset reads; only `kSpilled` branches to a var-heap fetch (one extra page touch, by design confined to oversize values).

## 7. Required Amendments (gate)

1. **`docs/spec/heap-and-tuple.md`:** tuple layout section rewritten to fixed cells + tagged cell format; stamp the date; link the quarry draft as the argument record.
2. **`docs/spec/wal.md`:** add `VARHEAP_APPEND` to the record catalog; note the §5 write-ordering and the no-special-recovery rule.
3. **Page-class enum / `docs/spec/page.md`:** add `kVarHeap` to the headered, logged classes; note relayout exemption.
4. **Superblock spec:** the pinned `inline_cell_width` field + startup validation rule.
5. **Row codec (`src/exec/row_codec.*`):** tagged-cell implementation; fixed-size assertion per relation.
6. **Client manual:** nothing user-visible changes except the absence of `VARCHAR(n)` syntax — state that explicitly.
7. **`CLAUDE.md`:** invariant-adjacent summary ("tuples are fixed-length; oversize values spill to the immutable var-heap") + §9 opens.

## 8. Testing Requirements

1. **Cell codec:** round-trips for all three tags; boundary at `len = W − 3` and `W − 2`; zero-padding verified (no stale bytes leak between versions).
2. **The property that names the feature:** a randomized UPDATE workload (values oscillating across the spill boundary) never changes any tuple's `(page_id, slot)` — instrumented, zero moves.
3. **MVCC over spills:** old-version readers resolve old pointers correctly while newer versions exist; purge reclaims exactly the dead values (oracle count).
4. **Crash matrix:** injected crashes between `VARHEAP_APPEND` and the cell overwrite, and during purge reclamation — recovery via ordinary replay; unreferenced values swept; replaying twice is a no-op.
5. **Config pinning:** superblock/configuration mismatch refuses startup with both values named; fresh bootstrap honors the configured width.
6. **Invisibility:** wire-level golden sessions produce byte-identical results for inline vs spilled storage of the same logical value.
7. **Advisory family unaffected:** the standing Waystone-off/dropped-trails equivalence suite passes over spilled-value workloads.

## 8a. Implementation status — phase 1 landed 2026-08-01

**Phase 1 is the rule without the var-heap.** Invariant 13 holds in code: a
relation's row size is a schema constant, tuple addresses are stable across
UPDATE, and the width is instance-pinned. What phase 1 does *not* do is
spill — a value too long to inline is refused with `Unsupported` naming the
var-heap, where before the rule it would have been stored (up to 65535
bytes) as a variable-length field.

Landed:

| Gate item | Where |
|---|---|
| §7.1 `heap-and-tuple.md` | already written; §3.4 now marks spilling as specified-not-implemented |
| §7.4 superblock pin + startup validation | `include/kds/server/superblock.hpp` (`inline_cell_width` at body offset 36, replacing `reserved2`; format version 3 → 4), `src/bootstrap/bootstrap.cpp` |
| §7.5 row codec | `include/kds/storage/tagged_cell.hpp` (the cell format), `catalog::RowLayout` in `include/kds/catalog/schema.hpp` (the constant), `src/exec/row_codec.cpp` (static offsets) |
| §7.6 client manual | done, including the explicit "there is no `VARCHAR(n)`" |
| §7.7 `CLAUDE.md` | done |
| §8.1, §8.2, §8.5, §8.6 tests | `tests/tagged_cell_test.cpp`, `tests/row_layout_test.cpp`, `tests/fixed_length_tuple_test.cpp`, `tests/bootstrap_test.cpp` |

## 8b. Implementation status — phase 2 (the var-heap) landed 2026-08-01

The remaining gate items are done. A value too long to inline now **spills**
rather than being refused, and storage is invisible above the codec.

| Gate item | Where |
|---|---|
| §7.2 `VARHEAP_APPEND` in the WAL record catalog | `RecordType::kVarHeapAppend = 16`, `include/kds/wal/payload.hpp`, `docs/spec/wal.md` §5.2 |
| §7.3 `kVarHeap` in the page-class enum | `PageType::kVarHeap = 10`, `include/kds/storage/varheap.hpp`, `docs/spec/page.md` §5a |
| The spill path | `storage::EncodeSpilledCell` + `varheap::ChainAppend`, driven from `EncodeRow`'s `VarHeapSink` |
| Catalog row change | `sys.tables` gained `varheap_page_id`, growing `SysTableRow::kOnDiskSize` — so the superblock format version went **4 → 5**. The row is not part of the superblock, but a catalog row format change is just as breaking, and without the bump a phase-1 database mounted and then failed on its first catalog read with an opaque size mismatch. |
| The fetch path | `varheap::Fetch` via `exec::ResolveSpills` |
| §8.3 tests, as far as reachable | `tests/varheap_test.cpp`, `tests/fixed_length_tuple_test.cpp` |

Four design points worth having in one place:

- **Per-relation chain, root fixed at `CREATE TABLE`.** `sys.tables` gained
  `varheap_page_id`, allocated eagerly for any schema that can spill and
  `kInvalidPageId` otherwise, so a relation of plain integers costs no
  var-heap page. Eager rather than on-first-spill because a lazily
  allocated root would be a fact changing *without DDL*, and
  `catalog_cache.hpp`'s rule says such a fact may not be cached — while
  this one is cached on every `TableAccess`. Chain growth edits the tail's
  link, never the root, so the root stays DDL-immutable.
- **Decode does not resolve; it reports.** `DecodeRowInto` records a
  spilled cell as a *pending* spill and the caller fetches afterwards
  through `ResolveSpills`. This is `parser-v2.md` I15's rule R1 — no
  page-frame span live across a nested fetch — and resolving inline would
  have put a var-heap fetch under exactly the span the step VM's
  `PageSpanGuard` exists to catch. A row with nothing spilled pays nothing
  for the split.
- **`VARHEAP_APPEND` precedes the `HEAP_INSERT` that points at it.** That
  direction is the recovery story: a replay must never reach a cell whose
  pointer resolves to nothing, whereas a value with no tuple is an
  unreferenced value purge collects. No var-heap-specific recovery logic
  exists, per §5.
- **Max value is one page, 8144 bytes.** This is *not* the §9 cap being
  decided — a larger value needs a multi-page representation, and inventing
  one to answer an open question is what §9 forbids. Refused with
  `Unsupported`; a future cap can be lower (policy above the layer) or
  higher (chaining behind the same `Append`/`Fetch` pair).

Still owed, and both blocked on machinery that does not exist:

- **Nothing reclaims.** Reclamation rides on purge (§5) and there is no
  purge, so a superseded value's bytes stay until there is. An UPDATE that
  shortens a spilled value abandons the old one.
- §8.4 (the crash matrix) and §8.7 (Waystone equivalence over spilled
  workloads) need recovery and trail replay respectively, neither of which
  is implemented.

Two decisions this pass forced, recorded because neither is in §0:

- **`float`/`decimal` columns are now refused at `CREATE TABLE`**
  (`catalog::RowLayout::Build`). A fixed row size has to reserve a width for
  every column, and neither type has a decided on-disk encoding; reserving
  one would be half of settling it. Before the rule they could be declared
  and never populated, which cost nothing because a row's size did not
  depend on them.
- **Var-heap layout, for phase 2 `[CONFIRMED 2026-08-01]`:** a
  **per-relation chain** rooted at a new `varheap_page_id` field in
  `sys.tables`, grown by tail append exactly as `heap_chain.cpp` grows a
  heap. Chosen over one instance-wide chain for per-relation locality and
  because `DROP TABLE`, when it exists, then reclaims one chain rather than
  sweeping a shared one. The cost is a catalog row format change.

Every existing data file stops mounting at superblock version 4. That is
V5 working as intended: there is no migration path while the format moves.

## 9. Open Items — do not assume

- **`kds.inline_cell_width` default value** (64 `[PROPOSED]`): settle against measured string-length distributions of target schemas; this is the number that decides whether common strings ever spill.
- Spilled-value size cap (uncapped blobs are not obviously an OLTP feature). **Unsettled, and the implementation does not settle it**: `varheap::ChainAppend` refuses anything larger than one page (8144 bytes) with `Unsupported`, because a bigger value needs a multi-page representation and inventing one would answer this question by accident. A future cap can be lower (a policy check above the layer) or higher (chaining behind the same `Append`/`Fetch` pair).
- V4 revisit trigger: adopt prefix-inline only if string-equality steps ever become a measured cost — recorded so the "no" has an exit condition.
- Purge-cadence sizing metric for var-heap headroom (ties into the observability set).
