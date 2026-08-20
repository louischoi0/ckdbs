# Workplan — NULL storage and semantics (NU series)

Status: **ratified 2026-08-20, build in progress.** The owning spec is
`docs/spec-null.md`, whose design was already decided (the tail null
bitmap sized to nullable columns, preserving invariant 13; the bitmap as
sole authority with `kNull` as defined filler). This file records the
ratified `[OPEN]`s and the task slices.

## Ratified decisions (spec §5, closed here)

- **D1 — NOT NULL is the default; `NULL` is the opt-in column modifier.**
  KDS-current semantics: every existing `CREATE TABLE` statement keeps
  its meaning (the truthfulness rule — never silently reinterpret), the
  zero-cost property holds for every schema that does not ask for NULLs,
  and the financial-OLTP shape is mostly `NOT NULL` anyway. `NOT NULL`
  also parses and is a no-op, so standard-minded schemas do not refuse.
  The divergence from the standard gets a loud note in `manual/sql/`.
  Declined: standard-conforming nullable-by-default — it silently
  changes already-written statements.
- **D2 — `CREATE INDEX` on a nullable key column is refused in v1**,
  with the byte position and the reason. The index entry format,
  maintenance, backfill and the golden contract suite stay untouched —
  an index still holds exactly one entry per row of a domain where the
  key always exists. `IS NULL` answers by scan. Declined: storing NULLs
  (an entry-format change through split logic and the contract corpus)
  and Oracle-style omission (a filtered maintenance path, and `IS NULL`
  can never probe). Revisit with a measured need, as `feat-index.md`
  §13's other opens are.
- **D3 — NULLs sort largest**: `ASC` puts them last, `DESC` first —
  PostgreSQL's rule, one fixed default, documented per direction. The
  `NULLS FIRST/LAST` grammar is deferred as its own small parser task.
  This also gives the sort path a **total order**, so `ORDER BY` keeps a
  boolean comparator while `WHERE` alone goes three-valued.

## The seams, named (surveyed at `080f73a`)

- `parser::ValueType::kNull` and the `kNullLit` token already exist; the
  storage `CellTag::kNull` writer/decoder exist unused.
- The one refusal to lift: `EncodeRow` at `src/exec/row_codec.cpp:115`.
  `DecodeRow` already maps a `kNull` tag to `ValueType::kNull` (:489) —
  under §3 it must read the **bitmap** instead, with tag/bitmap
  disagreement as `Corruption`.
- `CompareValues` (`row_codec.hpp:313`) returns bool and stays that way
  as the **total-order comparator** (NULL largest) for sort paths; the
  predicate paths gain a tri-state evaluation whose unknown never
  passes `WHERE`.
- `EncodeGroupKey` (`bound_cabin.hpp:100`) is the one key encoder both
  aggregation and assertions inherit — one NULL encoding, non-colliding
  with every real value, grouping NULL with NULL.

## Task slices

- **NU1** — this ratification; spec §2.3/§5 amended to decided. ☑
- **NU2** — layout: `RowLayout` gains the per-column null-bit index
  (`kNoNullBit` for `NOT NULL`), `null_bitmap_bytes`, `row_size` growth;
  the §6 layout property tests (0 bytes and byte-identical `row_size`
  for every all-`NOT NULL` schema; the 8/9-column byte boundary).
- **NU3** — grammar + catalog: the `NULL` column modifier (and no-op
  `NOT NULL`) in both CREATE TABLE paths; `notnull=false` written;
  refuse a nullable **first column** (invariant 11: the pk has no NULL
  encoding); refuse `CREATE INDEX` whose key includes a nullable column
  (D2); `DESCRIBE` already displays the flag.
- **NU4** — codec: `EncodeRow` accepts `kNull` for a nullable column
  (bit set, fixed cells zeroed, varchar cells written as `kNull` tag),
  refuses it for `NOT NULL` with the column named; `DecodeRow` reads the
  bitmap as sole authority; tag/bitmap disagreement is `Corruption`;
  round-trip tests per type and position.
- **NU5** — statements and predicates: INSERT/UPDATE with NULL;
  `IS NULL` / `IS NOT NULL`; three-valued comparison at every predicate
  site with `WHERE` keeping true only; the truth-table tests driven
  through the statement surface.
- **NU6** — aggregation and grouping: `COUNT(col)` skips, `COUNT(*)`
  does not; `SUM`/`MIN`/`MAX` skip; `AVG`'s denominator is the non-NULL
  count; an all-NULL group's `SUM` is NULL; `EncodeGroupKey`'s NULL
  encoding; assertions inherit it and a NULL `SUM` column contributes
  nothing.
- **NU7** — ordering, wire, foreign keys: `ORDER BY` under D3 (including
  the top-N heap and the pk-elided path's non-interaction — a pk is
  never NULL); the text protocol renders NULL distinguishably from `''`;
  a NULL child key satisfies its fkey vacuously (`kFkNullable`).
- **NU8** — the gates: contract suites byte-unchanged for all-`NOT NULL`
  relations; critics-developer per slice; ck-tester with the overhead
  question — the decode path's bitmap read must cost nothing when
  `null_bitmap_bytes == 0`, priced by interleaved A/B on the existing
  scenarios; docs (spec flips, `known-gaps.md`'s "No NULL storage",
  `CLAUDE.md` row, `manual/sql/`).

## Not in scope, by decision

`NULLS FIRST/LAST` grammar (D3); nullable index keys (D2); `ALTER TABLE
ADD COLUMN` of any kind (spec §5 records why the nullable case is a
rewrite, and AL1 refuses everything data-moving); `DEFAULT NULL` or any
DEFAULT clause; NULL parameters in patterns beyond what V08's `IN`-list
task owns.
