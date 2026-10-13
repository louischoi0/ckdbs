# NULL storage and semantics

How a KDS tuple records the absence of a value, and what that absence means
everywhere it is read.

**Status: built.** A relation may declare nullable columns and store NULLs
in them. The ratified rules, each owned by a section below: **NOT NULL is
the default and `NULL` is opt-in** (D1, §2.3), **no nullable column enters
a secondary index, covered columns included** (D2, §4), and **NULLs sort
largest** (D3, §4). Every all-`NOT NULL` relation keeps a byte-identical
row layout (zero bitmap bytes), so the feature carries no format bump and
no migration. This document is the owning spec; it **amends
`docs/rules/rule-fixed-length-tuple.md` §2** with one addition to the row
layout and leaves the rest of that rule untouched.

Companion specs: `docs/rules/rule-fixed-length-tuple.md` (the tagged cell
and the fixed-length rule), `docs/spec/heap-and-tuple.md` §3.3 (row layout),
`docs/spec/types.md`, `docs/spec/index.md`, `docs/spec/aggregate.md`,
`docs/spec/parser-v2.md`.

---

## 1. Why not Oracle's representation

Oracle's model — `[length][data]` per column, NULL as length `0xFF`, trailing
NULLs omitted — is rejected on two grounds, both correctness rather than taste:

- **It contradicts invariant 13.** A variable-length row breaks
  `docs/rules/rule-fixed-length-tuple.md` §2 (row size is a per-relation
  constant, asserted in the row codec), and with it in-page slot arithmetic,
  `PageView::OverwriteTuple()` always fitting, and the stable `(page_id, slot)`
  address that Waystone, the Cabin and the physical optimizer rely on.
- **It conflates `''` with NULL**, the defect
  `docs/rules/rule-fixed-length-tuple.md` §3 gives as the reason for the tag
  byte.

Its merit — a NULL costing no space — is kept by §2's route: a relation with
no nullable column pays nothing.

---

## 2. The rule (normative)

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

`RowLayout` carries one array positionally aligned with `Schema::columns`: the
bit index of each column, or the `kNoNullBit` sentinel for a `NOT NULL` column.
Derived in `RowLayout::Build()` from the schema alone, like every other member
it carries — which is what keeps a second, disagreeing notion of "which bit is
this column's" from being computed on an execute path.

**Trailing, not leading, because of the zero-fill**: every existing column
offset stays put, and a zero-filled payload reads as "nothing is NULL" —
what a row written before nullable columns existed means.

### 2.2 Why this costs an all-`NOT NULL` relation nothing

`SysColumnRow::notnull` is the flag, at `kNotNullOffset`, displayed by
`sys.columns` and `DESCRIBE`; under §2.3's default every column is
`notnull = true` unless it declared otherwise. A relation with no nullable
column therefore has a nullable-column count of zero and
`null_bitmap_bytes` of 0, which is the reason to size the bitmap by nullable
columns rather than by all columns:

- **No format break and no migration.** `row_size` is byte-identical for every
  relation that declares no nullable column, so no data file is rewritten and
  no superblock version moves.
- **Pay-per-use.** A 40-column relation with no nullable column pays 0 bytes,
  not 5.
- **No catalog change.** The flag, its offset, its display and its codec
  predate the feature; it added a grammar that sets it false and a row codec
  that honours it.

### 2.3 The `NOT NULL` grammar, and the default (D1)

**A column is `NOT NULL` unless declared `NULL`.** The per-column suffix
`[NULL | NOT NULL]` follows the type; a column that says nothing refuses
NULL, `NULL` opts it in, and `NOT NULL` spells the default out loud. This is
**a deliberate divergence from standard SQL**, where a column is nullable
unless declared `NOT NULL`: the standard's default would cost bitmap bytes on
every relation that does not say `NOT NULL` and would silently change what an
existing `CREATE TABLE` statement means, and the engine's rule is that
truthfulness beats convenience — a statement already written keeps its
meaning. Storing NULL into a column not declared `NULL` is refused by name,
with the byte.

---

## 3. `kNull` and the bitmap: one authority, not two

`storage::CellTag::kNull` exists, written by `storage::EncodeNullCell()` and
reported by `storage::DecodeCell()`. That leaves a varchar column with two candidate places to
record nullness, which is two things that can disagree.

**The rule: the bitmap decides; the tag is the defined filler.** A NULL varchar
cell is written as `kNull` — tag byte then zeros — so its bytes are
deterministic rather than stale, but `IS NULL` and every comparison read the
bitmap and never the tag. A cell whose tag says `kNull` while the bitmap says
present is `Corruption`, never interpreted, on the same footing as a payload
whose length disagrees with the schema constant.

Rejected alternative: the tag authoritative for variable-width columns and the
bitmap for fixed-width ones only. Declined because every NULL-aware operation
(`IS NULL`, three-valued comparison, `GROUP BY` encoding, aggregate skipping)
would carry two code paths that must agree, and "does this row hold any NULL?"
would stop being one masked read.

---

## 4. What NULL reaches, and what each part owes

Storage is the small half. The list below is the actual scope, and each item
belongs to the doc named.

- **Three-valued comparison.** `exec::CompareValues()` answers
  true/false/unknown, and every predicate site says what it does with
  unknown. `WHERE` keeps only true, so `col = NULL` matches nothing, ever;
  under `OR` and `NOT` (BU-S1) the unknown is carried in Kleene logic to
  the conjunct's root and collapsed there, so `NOT (col = 1)` is unknown,
  not true, where `col` is NULL (`parser-v2.md` I16); a subquery predicate
  under them (BU-S3) is a leaf carrying its own unknown, so
  `NOT (col IN (SELECT ...))` is unknown where `IN` is;
  `IS NULL` / `IS NOT NULL` is the one spelling that finds NULLs, answered
  from the stored bitmap.
- **`IN` / `NOT IN`** (`docs/spec/parser-v2.md`). `NOT IN` over a subquery
  result containing NULL yields no rows — the standard's tri-state collapse,
  not `!IN`. A NULL probe against a non-empty result is unknown for `IN`
  and `NOT IN` alike, so neither admits the row; against an empty result
  `NOT IN` is true whatever the probe. A value list (BU-S2) is read the
  same way: `IN (1, NULL)` is true only where the probe is 1, and
  `NOT IN (1, NULL)` is never true.
- **The primary key is never NULL**, and this is not a policy but invariant 11:
  the pk is carried by the Keystone word, which has no NULL encoding. A first
  column declared `NULL` is refused at `CREATE TABLE`.
- **Secondary indexes** (D2, `docs/spec/index.md`). **No nullable column
  enters an index**, as a key or as a covered column; `CREATE INDEX` refuses
  it by name. `IS NULL` therefore answers by scan.
- **Aggregates** (`docs/spec/aggregate.md`). `COUNT(*)` counts rows,
  `COUNT(col)` skips NULLs; `SUM`/`MIN`/`MAX` skip them; `AVG`'s denominator is
  the non-NULL count; an all-NULL group's `SUM` is NULL, not 0.
- **`GROUP BY`.** `exec::EncodeGroupKey()` encodes NULL so that it cannot
  collide with any real value, and NULL groups with NULL under the standard's
  "not distinct" rule — which is the opposite of `=` and is written down where
  the key is encoded.
- **Assertions** (`docs/spec/assertion.md`). A Bound Cabin group key derives
  from `EncodeGroupKey`, so it inherits the above; and a NULL in a `SUM` column
  contributes nothing, which the entry's inline aggregate value represents.
- **`ORDER BY`** (D3, `docs/spec/parser-v2.md`). **NULLs sort largest**: `ASC`
  puts them last, `DESC` first — one fixed default per direction. There is no
  `NULLS FIRST` / `NULLS LAST` grammar to override it. Two NULLs compare equal
  and the sort's sequence tiebreak keeps the order total.
- **Foreign keys** (`docs/spec/foreign-keys.md`). `kFkNullable` is stamped
  from the child column's declared nullability; a NULL child key satisfies
  the constraint vacuously (MATCH SIMPLE — the check is skipped).
- **The wire and the client** (`docs/spec/protocol.md`,
  `docs/spec/client-manual.md`). A NULL is distinguishable from the empty
  string on the way out — the same distinction §1 keeps in storage: KWP
  carries `{i32 len | -1 = NULL}` per value, and the text protocol renders
  the token `NULL`.

---

## 5. Open decisions

The decisions once listed here are ratified and stated normatively in §2.3
and §4. One consequence stands as a fact: `ALTER TABLE ADD COLUMN <nullable>`
is refused — it changes `row_size`, and under `docs/spec/alter.md` AL1
everything data-moving is refused.

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
  (waystone, index, cabin, inner-build) must be unchanged for all-`NOT NULL`
  relations — the regression that would say this feature leaked into relations
  that never asked for it. The types and aggregate contracts belong in the same
  sweep for the same reason, though they pin claims rather than configurations;
  assertion has no contract suite at all — `tests/assertion_*_test.cpp` are
  what carry the check there.
- **Three-valued logic**: a truth table per operator, driven through the
  statement surface, not the comparison function alone.
