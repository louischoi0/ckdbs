# NULL storage and semantics

How a KDS tuple records the absence of a value, and what that absence means
everywhere it is read. `[PROPOSED]` marks a default to confirm or amend before
the affected part is built; `[OPEN]` must not be assumed.

**Status: built 2026-08-20** (`docs/workplan-null.md` NU1-NU8 carries the
task record and the ratified decisions - NOT NULL default with `NULL`
opt-in, nullable index keys refused in v1 covered columns included, NULLs
sort largest). A relation may now declare nullable columns and store NULLs
in them; every all-`NOT NULL` relation keeps a byte-identical row layout
(zero bitmap bytes), which is what made the feature land with no format
bump and no migration. This document is the owning spec; it **amends
`docs/rules/rule-fixed-length-tuple.md` §2** with one addition to the row layout
and leaves the rest of that rule untouched.

Companion specs: `docs/rules/rule-fixed-length-tuple.md` (the tagged cell and the
fixed-length rule), `docs/spec/heap-and-tuple.md` §3.3 (row layout),
`docs/spec/types.md`, `docs/spec/index.md`, `docs/spec/aggregate.md`,
`docs/inflight/in-progress/parser-v2-workplan.md` V08.

---

## 1. Why not Oracle's representation

The obvious model is Oracle's, and it is the one this spec rejects. Recorded
because it is the first thing anyone proposes, and the reasons are not matters
of taste.

Oracle stores each column as `[length][data]`, encodes NULL as a length byte of
`0xFF` with no data, and **omits trailing NULLs entirely** — the row simply
ends early. That is where its space win comes from, and it makes the row
variable-length.

**It contradicts invariant 13.** `docs/rules/rule-fixed-length-tuple.md` §2: a
relation's row size is a per-relation constant, and *"no code path may produce
a tuple whose size differs from its relation's constant. This is asserted in
the row codec, not policed by convention."* The invariant is not decoration —
it is what makes in-page slot addressing arithmetic, what lets
`PageView::OverwriteTuple()` always fit so an UPDATE cannot migrate a tuple,
and therefore what makes a `(page_id, slot)` pair a stable address for the life
of a row. Waystone trails, the Cabin's location hints and the physical
optimizer are all built on that stability. Adopting a variable-length row means
retracting invariant 13 and everything standing on it.

**It re-introduces a defect this engine already designed around.** Because a
zero-length value *is* Oracle's NULL encoding, Oracle cannot distinguish `''`
from NULL in `VARCHAR2`. `docs/rules/rule-fixed-length-tuple.md` §3 gives exactly
that case as the *rationale* for the tag byte: *"NULL, empty string, and
spilled must be distinguishable without reading the var-heap."* The tag exists
because someone already declined this trade.

What Oracle's model does have — a NULL costing no space — is worth keeping as a
goal, and §3 keeps most of it by a different route: a relation with no nullable
column pays nothing at all.

---

## 2. The rule (normative) `[PROPOSED]`

- Every relation's row carries a **null bitmap**: a fixed run of bytes at the
  end of the tuple payload, sized from the schema alone.
- The bitmap has one bit per **nullable** column — a column whose
  `sys.columns.notnull` is false — in ascending schema position. Columns
  declared `NOT NULL` consume no bit.
- `null_bitmap_bytes = ceil(nullable_column_count / 8)`, and is **0 when the
  relation has no nullable column**.
- A set bit means the column is NULL. The payload is zero-filled before
  encoding, so all-zero means all-present.
- The bitmap is the **sole authority** on whether a column is NULL. No reader
  infers nullness from a column's value bytes.
- `RowLayout::row_size` grows by `null_bitmap_bytes` and by nothing else.
  Column offsets are unchanged: the bitmap is appended, so `offsets[0]` is
  still 0 and the Keystone word still leads every tuple.

### 2.1 Layout

For a relation whose payload is `row_size` bytes:

| Region | Bytes | Notes |
|---|---|---|
| Keystone word | 8 | `offsets[0] == 0`, unchanged (invariant 5) |
| columns | per `RowLayout::offsets` | unchanged widths, unchanged offsets |
| null bitmap | `null_bitmap_bytes` | at `row_size − null_bitmap_bytes` |

Bit `i` is the `i`-th nullable column in ascending `pos`; it lives in byte
`i / 8` at bit `i % 8`, counting from the least significant bit — the same
explicit shift-and-mask discipline every persisted structure here uses
(invariant 6: no compiler bitfields in a persisted format).

`RowLayout` gains one array positionally aligned with `Schema::columns`: the
bit index of each column, or a `kNoNullBit` sentinel for a `NOT NULL` column.
Derived in `RowLayout::Build()` from the schema alone, like every other member
it carries — which is what keeps a second, disagreeing notion of "which bit is
this column's" from being computed on an execute path.

**Trailing, not leading, and the reason is the zero-fill.** Placing the bitmap
after the columns leaves every existing column offset where it is, and a
zero-filled payload reads as "nothing is NULL" — which is exactly what a row
written before this feature means. The same argument that let `group_id` ride
in the Bound Cabin entry's zero padding (`docs/spec/assertion.md` §5.1).

### 2.2 Why this costs existing data nothing

`SysColumnRow::notnull` **already exists** (`include/kds/catalog/rows.hpp:188`,
at `kNotNullOffset`), is already displayed by `sys.columns` and `DESCRIBE`, and
is written `true` for every column by every path that creates one —
`src/server/command_dispatcher.cpp:1974` does so with the comment *"no NULL
support yet"*. So **every column of every relation in existence is
`notnull = true`**, every relation has a nullable-column count of zero, and
`null_bitmap_bytes` is therefore 0 for all of them.

The consequences are worth stating plainly, because they are the reason to size
the bitmap by nullable columns rather than by all columns:

- **No format break and no migration.** `row_size` is byte-identical for every
  relation that exists today, so no data file needs rewriting and no superblock
  version has to move. Contrast the decimal `(precision, scale)` decision
  (`docs/spec/types.md` TY9), which rode inside an existing field precisely
  because widening `SysColumnRow` would have stopped every pre-existing data
  file from mounting.
- **Pay-per-use.** A 40-column relation with no nullable column pays 0 bytes,
  not 5.
- **No catalog change at all.** The flag, its on-disk offset, its display and
  its codec are already there. What is missing is a grammar that can set it
  false and a row codec that honours it.

### 2.3 The `NOT NULL` grammar, and the default — **decided 2026-08-20: KDS-current** (`workplan-null.md` D1)

Setting `notnull = false` needs a spelling, and choosing it is a decision this
document does **not** take:

- **Standard-conforming**: a column is nullable unless declared `NOT NULL`.
  Costs bitmap bytes on every new relation that does not say `NOT NULL`, and
  silently changes what an existing `CREATE TABLE` statement means.
- **KDS-current**: a column is `NOT NULL` unless declared `NULL`. Preserves
  today's behaviour exactly and keeps the zero-cost property for anyone who
  does not ask for NULLs, at the price of diverging from the standard on a
  point users will not expect to be divergent.

The engine's stated rule is that truthfulness beats convenience and a refusal
carries a byte position (`CLAUDE.md`), which argues against silently
reinterpreting statements already written. It does not settle the question.
**Flag it; do not assume it.**

---

## 3. `kNull` and the bitmap: one authority, not two

`storage::CellTag::kNull` already exists
(`include/kds/storage/tagged_cell.hpp:95`), is written by `WriteNullCell()` and
reported by `DecodeCell()`; nothing calls it, and the file says so. That leaves
a varchar column with two candidate places to record nullness, which is two
things that can disagree.

**The rule: the bitmap decides; the tag is the defined filler.** A NULL varchar
cell is written as `kNull` — tag byte then zeros — so its bytes are
deterministic rather than stale, but `IS NULL` and every comparison read the
bitmap and never the tag. A cell whose tag says `kNull` while the bitmap says
present is `Corruption`, never interpreted, on the same footing as a payload
whose length disagrees with the schema constant.

The rejected alternative, recorded with its real merit: let the tag be
authoritative for variable-width columns and the bitmap cover only fixed-width
ones. It gives each column exactly one home for its nullness and a smaller
bitmap, and a reader already dispatches on column type anyway. It is declined
because every NULL-aware operation — `IS NULL`, three-valued comparison,
`GROUP BY` key encoding, aggregate skipping — would then carry two code paths
that must agree, and the cheap whole-row question ("does this row contain any
NULL at all?") stops being one masked read.

---

## 4. What NULL reaches, and what each part owes

Storage is the small half. The list below is the actual scope, and each item
belongs to the doc named — a workplan should not discover them one at a time.

- **Three-valued comparison.** `exec::CompareValues()` becomes
  true/false/unknown, and every predicate site has to say what it does with
  unknown. `WHERE` keeps only true.
- **`IN` / `NOT IN` (V08, `docs/inflight/in-progress/parser-v2-workplan.md`).** `NOT IN` over a list
  containing NULL yields no rows — the tri-state collapse `row_codec.hpp:50`
  already names as the reason NULLs and this task are linked. `IN (list)` is
  unbuilt, so it should be built NULL-aware rather than retrofitted.
- **The primary key is never NULL**, and this is not a policy but invariant 11:
  the pk is carried by the Keystone word, which has no NULL encoding. Refuse at
  `CREATE TABLE` if the first column is declared nullable.
- **Secondary indexes** (`docs/spec/index.md` §13). Whether a NULL key is
  stored at all, and where it sorts. Oracle omits NULLs from B-tree indexes
  entirely, which is why `IS NULL` cannot use one there — a real trade, not an
  oversight. `[OPEN]`.
- **Aggregates** (`docs/spec/aggregate.md`). `COUNT(*)` counts rows,
  `COUNT(col)` skips NULLs; `SUM`/`MIN`/`MAX` skip them; `AVG`'s denominator is
  the non-NULL count; an all-NULL group's `SUM` is NULL, not 0.
- **`GROUP BY`.** `exec::EncodeGroupKey()` needs a NULL encoding that cannot
  collide with any real value, and NULL groups with NULL under the standard's
  "not distinct" rule — which is the opposite of `=` and has to be written
  down where the key is encoded.
- **Assertions** (`docs/spec/assertion.md`). A Bound Cabin group key derives
  from `EncodeGroupKey`, so it inherits the above; and a NULL in a `SUM` column
  contributes nothing, which the entry's inline aggregate value must represent.
- **`ORDER BY`** (`docs/workplan-order-by.md`). `NULLS FIRST` / `NULLS LAST`,
  and what the default is per direction. `[OPEN]`.
- **Foreign keys** (`docs/spec/foreign-keys.md`). Already anticipated:
  `kFkNullable` exists at `include/kds/catalog/rows.hpp:841`. A NULL child key
  satisfies the constraint vacuously.
- **The wire and the client** (`docs/spec/client-manual.md`,
  `docs/inflight/in-progress/protocol-wp.md`). The text protocol needs a rendering for NULL
  distinguishable from the empty string, which is the same distinction §1 keeps
  in storage and must not be lost on the way out.

---

## 5. Open decisions — ratified 2026-08-20 (`workplan-null.md`), kept as the option record

- The nullability default and its grammar (§2.3).
- Whether a NULL key enters a secondary index, and its sort position (§4).
- `ORDER BY` NULL ordering and its per-direction default (§4).
- Whether `ALTER TABLE ADD COLUMN <nullable>` is ever allowed. It changes
  `row_size`, so under `docs/spec/alter.md` AL1 — catalog-only renames,
  everything data-moving refused — it is refused today, and this spec does not
  change that. Recorded because "add a nullable column" is the one ALTER users
  expect to be free, and here it is a rewrite.

## 6. Testing requirements

- **Layout**: `RowLayout::Build()` produces `null_bitmap_bytes == 0` and a
  byte-identical `row_size` for every all-`NOT NULL` schema — the property §2.2
  claims, asserted rather than believed.
- **Round-trip**: encode/decode of every type with a NULL in each position,
  including first and last nullable column, and the byte boundary at 8 and 9
  nullable columns.
- **The disagreement is Corruption**: a varchar cell tagged `kNull` whose
  bitmap bit is clear fails loudly (§3).
- **Contract suite**: the existing byte-for-byte configuration comparisons
  (waystone, index, cabin, types, assertion) must be unchanged for all-`NOT
  NULL` relations — the regression that would say this feature leaked into
  relations that never asked for it.
- **Three-valued logic**: a truth table per operator, driven through the
  statement surface, not the comparison function alone.
