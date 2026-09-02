# Fixed-Length Tuples & the Var-Heap — Technical Specification

**Status:** **Official specification.** Rationale is retained inline — every decision here carries its *why*, and this file is its own argument record. Markers: `[CONFIRMED]`, `[PROPOSED]`, `[OPEN]`. Consistent with `docs/rules/rules.md`, `docs/spec/heap-and-tuple.md`, `docs/spec/wal.md`, `docs/spec/txn.md`, `docs/spec/types.md`, `docs/spec/waystone-concpets.md`, and `docs/spec/physical-optimizer.md`.

## 0. Decision Record `[CONFIRMED]`

| # | Decision | Choice |
|---|---|---|
| V0 | The rule | **Every tuple is fixed-length.** Variable-width types occupy fixed-size tagged cells; oversize values live out of line in the **var-heap** |
| V1 | Inline width | **One quantity, `kds.inline_cell_width`**: an instance constant pinned at bootstrap is the default, and `varchar(N)` declares the same quantity for one column (§4). No second threshold and no second name for it |
| V2 | Var-heap shape | **Immutable per version**: updates write a new value and swap the pointer; var-heap bytes are never rewritten or moved in place |
| V3 | Var-heap durability | **Logged, headered, checksummed** — an ordinary authoritative page class (`kVarHeap`), not an advisory one |
| V4 | Prefix-inline for spilled values | **No** (§9) |
| V5 | Schema evolution | Changing a cell's width on existing data rewrites every row of the relation, so it is **`Unsupported`**: the instance constant cannot change on an existing file, and there is no `ALTER … TYPE varchar(M)` |

## 1. Background & Rationale

Two confirmed positions make tuple mobility a first-order concern in KDS. Relayout is the product — the physical optimizer's value compounds with anything that makes moving tuples cheaper or unintended movement rarer. And trails bet on location stability — every Waystone entry is a recorded `(page_id, slot, epoch)`, so the trail hit rate is a direct function of how often tuples sit still.

Variable-length rows attack exactly this: an UPDATE that grows a row can force it to move (Postgres pays as broken HOT chains, InnoDB as row migration), and in KDS the same event would additionally burn trail entries through epoch churn. Fixing the tuple length removes the disease at the root: **an UPDATE can never migrate a tuple**, and combined with the immutable `min_key`, a tuple's address is stable for life — until the physical optimizer moves it on purpose.

Secondary gains, each real: relayout becomes cell-`memcpy` with exact fill-factor math; in-page slot addressing becomes index arithmetic; the row codec reads static offsets (the engine's existing fixed-record discipline — Waystone entries, trail pages, frame headers — extended to user tuples); and the threshold at which relayout pays drops because moves got cheaper.

The honest cost, accepted knowingly: variable-length management is **relocated, not eliminated** — it moves into the var-heap (§5), and fixed cells spend space on padding for short values. The acceptance argument: the hot heap is where fixed length matters; the var-heap is deliberately boring (V2 makes it immovable); and on target OLTP schemas — short, uniform strings — a sane inline width keeps the common case entirely inline.

## 2. The Rule (normative)

- A relation's tuple layout is a sequence of fixed-size cells at offsets computable from the schema alone. Row size is a per-relation constant; in-page slot addressing is arithmetic.
- Fixed-width types (integers, bool, dates, timestamps, decimals, `char(N)`, the Keystone word, the MVCC header) occupy their natural widths.
- Every variable-width type (`TEXT`/`VARCHAR`, future blobs) occupies exactly **one tagged cell** (§3), regardless of the value stored. Its width is `kds.inline_cell_width` — the instance's, or **the column's own** when it was declared `varchar(N)` (§4).
- No code path may produce a tuple whose size differs from its relation's constant. This is asserted in the row codec, not policed by convention.

## 3. The Tagged Cell `[CONFIRMED format; widths PROPOSED]`

Cell width `W = kds.inline_cell_width`, a **per-column** number with an instance default (§4). Layout (memcpy codec, `static_assert`ed, LE — rules.md §2/§5):

| Tag (`u8` at offset 0) | Layout after tag | Meaning |
|---|---|---|
| `kNull` | zeros | SQL NULL; wire NULL convention maps 1:1 |
| `kInline` | `len u16`, then `len` bytes, zero padding | value fits: `len ≤ W − 3` |
| `kSpilled` | `len u32`, `varheap_ptr u64` (`page_id u32 · slot u16 · reserved u16`) | bytes live in the var-heap |

- The inline capacity is therefore `W − 3`; the spill decision is a pure function of value length **and of that column's `W`** — no heuristics, no per-row variance. Two `varchar` columns in one row may have different `W` and therefore different spill points; neither can vary row to row.
- An UPDATE that crosses the boundary in either direction changes the cell's *tag*, never the tuple's size.
- Rationale for a tag byte over sentinels: NULL, empty string, and spilled must be distinguishable without reading the var-heap, and the tag is where future cell kinds (V4 revisit) land without a format bump.

## 4. The Instance Default and the Column Override `[CONFIRMED semantics; default PROPOSED]`

`kds.inline_cell_width` is configuration-referenced but **instance-pinned**: read from configuration once at bootstrap, written into the superblock, and validated at every startup — a running configuration that disagrees with the superblock refuses to start (`InvalidArgument`, naming both values). It cannot be hot-changed; on-disk tuple layout depends on it, so changing it for existing data is a rebuild, which is `Unsupported` (V5).

It is the *default* a `varchar` column takes, and `varchar(N)` overrides it for that column:

- `N` **is** that column's `kds.inline_cell_width`. Same unit (cell bytes, tag and length included), same capacity formula `N − 3`, same three tags, same spill path. **There is no second threshold and no second name for one** — a review that finds a `max_inline_char_size` or any equivalent has found a defect.
- `N` is validated by `storage::CheckInlineCellWidth` and nothing else, so the bounds are the instance setting's: `[16, 4096]`. `varchar(8)` is therefore refused — the narrowest cell must still hold a 13-byte spilled descriptor (§3). That wart is accepted rather than patched with a second validator.
- `N` is **not a length cap**. A value longer than `N − 3` spills; it is not refused. The only length refusal is one var-heap page (8144 bytes, §8b).
- A bare `varchar` stores `len = 0` in `sys.columns` and reads at the instance width. 0 has always meant the instance width, so a file written before the override existed mounts byte-identical; `varchar(64)` under a 64-byte instance and a bare `varchar` are the same column in every byte but that one.
- The width rides in `SysColumnRow::len`, the field that also carries a decimal's `(p, s)` and `char`'s width (`docs/spec/types.md` §4a), so it costs no catalog format change.

**V5 holds on its true reason.** `ALTER … TYPE varchar(M)` is out because changing a cell's width rewrites every row of the relation (§2's constant) — not because no per-column width exists to widen. The row codec never reads the instance width per cell: it derives every cell's span from `RowLayout::offsets` (`CellOf`/`MutableCellOf`), so a per-column width threads through nothing below `RowLayout::ColumnWidth`.

Default: **64 bytes** `[PROPOSED]` — chosen so common OLTP strings (codes, names, references) never touch the var-heap; the counter-cost is 64 B per *undeclared* string column per row, which a declared width is the way to avoid. The default's value is §9's; the *semantics* above are confirmed regardless of the number.

## 5. The Var-Heap `[CONFIRMED]`

The out-of-line value store. Its design goal is to be **boring**: the mobility problem was removed from the heap and must not reappear here.

- **Immutable per version (V2).** Writing a spilled value appends `{len, bytes}` to a var-heap page and returns its pointer. Values are never rewritten and never moved. Consequences, which are the rationale: MVCC correctness is free — an old-version reader follows the old pointer to bytes that cannot have changed; pointers need no epoch, no validation, no forwarding; the var-heap is **relayout-exempt by construction** (the physical optimizer never touches `kVarHeap` pages); reclamation is not new machinery but a rider on purge — when a version dies, its values die with it. The accepted cost: churn-heavy string updates consume space until purge catches up, making purge cadence a sizing input.
- **Logged and headered (V3).** `kVarHeap` is a headered page class: common header, `page_lsn`, CRC32C, full WAL participation via a `VARHEAP_APPEND` record. Stated because the reflex runs the other way: waystone/trail pages are advisory, but a var-heap value is **authoritative data** — losing one loses a committed value, not a hint. The advisory rules do not apply and must not be pattern-matched onto this class.
- Write ordering on the update path: `UNDO_WRITE{kVarHeapAppend}` (the append's own rollback) → `VARHEAP_APPEND` (new value) → heap cell overwrite (`HEAP_OVERWRITE`, old cell image into undo) — all in the same transaction, replayed by the ordinary winner/loser machinery. The undo record comes first because redo alone must never resurrect an append the undo phase has no record to release. A crash between the append and the tuple write leaves no orphan: the append is a link in the transaction's own chain, so recovery's undo phase reaches it like any other loser write. No special recovery logic exists for the var-heap, which is what makes that possible rather than what it costs.

## 6. Interactions with Confirmed Design

- **min_key heap:** strengthened — tuple addresses change only under deliberate relayout.
- **MVCC (`trx_id` + `undo_ptr`):** unchanged; the undo record's old-cell image is fixed-size like everything else, and under V2 it is just the old tag+bytes-or-pointer.
- **Trails/Waystone:** pure beneficiary — fewer epoch bumps, higher validated-hit rates; no format impact.
- **Physical optimizer:** moves get cheaper (cell memcpy) and `kVarHeap` is explicitly outside its jurisdiction.
- **Parser/DDL:** `varchar(N)` and `char(N)` are the only width syntax (`docs/spec/types.md` §2b); there is no `ALTER … TYPE` (V5). `TEXT` stays as-is.
- **Wire protocol:** invisible, and must remain so: `TEXT` on the wire is length-prefixed bytes regardless of inline/spilled storage.
- **Row codec/executor:** static-offset reads; only `kSpilled` branches to a var-heap fetch (one extra page touch, by design confined to oversize values).

## 7. Required Amendments (gate)

All landed. The rule's other homes: `docs/spec/heap-and-tuple.md` §3.3–§3.4 (tuple layout and the var-heap), `docs/spec/wal.md` §5.2 (`VARHEAP_APPEND` and the §5 ordering), `docs/spec/page.md` §5a (`kVarHeap`, relayout-exempt), the superblock's pinned `inline_cell_width` and its startup validation (`include/kds/server/superblock.hpp`, `src/bootstrap/bootstrap.cpp`), the row codec (`src/exec/row_codec.cpp`), and `docs/spec/client-manual.md`.

## 8. Testing Requirements

1. **Cell codec:** round-trips for all three tags; boundary at `len = W − 3` and `W − 2`; zero-padding verified (no stale bytes leak between versions).
2. **The property that names the feature:** a randomized UPDATE workload (values oscillating across the spill boundary) never changes any tuple's `(page_id, slot)` — instrumented, zero moves.
3. **MVCC over spills:** old-version readers resolve old pointers correctly while newer versions exist; purge reclaims exactly the dead values (oracle count).
4. **Crash matrix:** injected crashes between `VARHEAP_APPEND` and the cell overwrite, and during purge reclamation — recovery via ordinary replay; unreferenced values swept; replaying twice is a no-op.
5. **Config pinning:** superblock/configuration mismatch refuses startup with both values named; fresh bootstrap honors the configured width.
6. **Invisibility:** wire-level golden sessions produce byte-identical results for inline vs spilled storage of the same logical value.
7. **Advisory family unaffected:** the standing Waystone-off/dropped-trails equivalence suite passes over spilled-value workloads.

## 8a. Implementation status — the rule in code

Invariant 13 holds in code: a relation's row size is a schema constant, tuple addresses are stable across UPDATE, and the width is instance-pinned with a per-column override.

| Piece | Where |
|---|---|
| Superblock pin + startup validation (§4) | `include/kds/server/superblock.hpp` (`inline_cell_width` in the body), `src/bootstrap/bootstrap.cpp` |
| The cell format (§3) | `include/kds/storage/tagged_cell.hpp` |
| The row constant (§2) | `catalog::RowLayout` in `include/kds/catalog/schema.hpp` |
| Static-offset codec | `src/exec/row_codec.cpp` |
| Tests §8.1, §8.2, §8.5, §8.6 | `tests/tagged_cell_test.cpp`, `tests/row_layout_test.cpp`, `tests/fixed_length_tuple_test.cpp`, `tests/bootstrap_test.cpp` |

## 8b. Implementation status — the var-heap in code

A value too long to inline spills, and storage is invisible above the codec.

| Piece | Where |
|---|---|
| `VARHEAP_APPEND` | `RecordType::kVarHeapAppend = 16`, `include/kds/wal/payload.hpp`, `docs/spec/wal.md` §5.2 |
| `kVarHeap` | `PageType::kVarHeap = 10`, `include/kds/storage/varheap.hpp`, `docs/spec/page.md` §5a |
| The spill path | `storage::EncodeSpilledCell` + `varheap::ChainAppend`, driven from `EncodeRow`'s `VarHeapSink` |
| The chain root | `sys.tables.varheap_page_id` |
| The fetch path | `varheap::Fetch` via `exec::ResolveSpills` |
| Tests §8.3 | `tests/varheap_test.cpp`, `tests/fixed_length_tuple_test.cpp` |

Four rules the implementation carries:

- **Per-relation chain, root fixed at `CREATE TABLE`.** `varheap_page_id` is allocated eagerly for any schema that can spill and is `kInvalidPageId` otherwise, so a relation of plain integers costs no var-heap page. Eager because a lazily allocated root would be a fact changing *without DDL*, which `catalog_cache.hpp`'s rule says may not be cached — and this one is cached on every `TableAccess`. Chain growth edits the tail's link, never the root, so the root stays DDL-immutable. A per-relation chain rather than one instance-wide chain: per-relation locality, and `DROP TABLE` reclaims one chain rather than sweeping a shared one.
- **Decode does not resolve; it reports.** `DecodeRowInto` records a spilled cell as a *pending* spill and the caller fetches afterwards through `ResolveSpills` — `parser-v2.md` I15's rule R1, no page-frame span live across a nested fetch, which the step VM's `PageSpanGuard` exists to catch. A row with nothing spilled pays nothing for the split.
- **`VARHEAP_APPEND` precedes the `HEAP_INSERT` that points at it.** A replay must never reach a cell whose pointer resolves to nothing, whereas a value with no tuple is merely unreferenced. No var-heap-specific recovery logic exists (§5).
- **Max value is one page, 8144 bytes**, refused `Unsupported` by `varheap::ChainAppend`. This is *not* the §9 cap being decided: a larger value needs a multi-page representation, and a future cap can be lower (a policy check above the layer) or higher (chaining behind the same `Append`/`Fetch` pair).

`float` columns are refused at `CREATE TABLE` (`catalog::RowLayout::Build`): a fixed row size has to reserve a width for every column, and `float` has no decided on-disk encoding.

## 9. Open Items — do not assume

The decisions here are unrecorded in this file: the value of the `kds.inline_cell_width` default (64 `[PROPOSED]`); the spilled-value size cap (uncapped blobs are not obviously an OLTP feature — today's one-page limit, §8b, is the representation's and not this decision); V4's revisit (prefix-inline only if string-equality steps become a measured cost); and the purge-cadence sizing metric for var-heap headroom.
