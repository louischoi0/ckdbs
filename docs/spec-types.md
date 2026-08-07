# KDS Types — DATE, TIMESTAMP, DECIMAL (Specification)

**Status:** Official specification, decisions confirmed 2026-08-06 (TY1–TY9, §0).
Lifts the CREATE TABLE refusal of `decimal` (`client-manual.md` §3) for the
fixed-width form specified here; `float` stays refused. Companion tasks:
`docs/types-workplan.md`. Markers: `[CONFIRMED]`, `[PROPOSED]`, `[OPEN]`.
Consistent with `docs/rule-fixed-length-tuple.md` (invariant 13),
`docs/parser-v2.md`, `docs/aggregate.md` (AG-series), `docs/heap-and-tuple.md`,
`docs/feat-cabin.md`.

## 0. Decision Record `[CONFIRMED 2026-08-06]`

| # | Decision | Choice |
|---|---|---|
| TY1 | v1 types | **`DATE`, `TIMESTAMP`, `DECIMAL(p,s)`** — all fixed-width, so invariant 13 is untouched. `FLOAT64` stays out (`Unsupported` at CREATE TABLE, reserved `kTypeValFloat = 6` unchanged): IEEE comparison and aggregation semantics conflict with the engine's exactness discipline, and nothing in it is needed to unblock the workloads that asked. `TIME`, `INTERVAL`, `timestamptz`: not reserved, not parsed |
| TY2 | DECIMAL representation | **Scaled int64**: the unscaled value in 8 bytes, `1 ≤ p ≤ 18`, `0 ≤ s ≤ p`. Comparison, grouping and SUM reuse the checked-int64 machinery verbatim (AG3). A variable-width numeric violates invariant 13 by construction; `p > 18` is a *future separate type* (int128, 16 bytes — a different schema constant, so the two can coexist), never a widening of this one |
| TY3 | Literals | **Quoted string literals only** in v1: `'2026-08-06'`, `'12.34'`. The column's `type_val` decides the interpretation. **The lexer does not change** — no decimal-point token, no date token — so every previously-accepted statement lexes identically and the fingerprint argument is structural: `kFingerprintVersion` unmoved. A bare `12.34` numeric token is phase 2, gated on its own fingerprint analysis |
| TY4 | Time encoding | `DATE` = **days since 1970-01-01, int32** (4 bytes). `TIMESTAMP` = **microseconds since the epoch, UTC, int64** (8 bytes). **Storage is always UTC**; there is no session time zone, no conversion, no `timestamptz` — rendering what UTC means locally is the client's act, and this is a documented product constraint, not a gap |
| TY5 | Value model | `DATE`/`TIMESTAMP` **reuse `AstValue` kInt** — the value *is* an integer; only its rendering differs, and rendering happens at the emission boundary (§4), not in the value. **`DECIMAL` alone adds a kind**: `kDecimal` carrying the unscaled int64 plus its scale. One new kind, not three, is what keeps every switch over `ValueType` from growing three arms that behave identically |
| TY6 | Mixed-scale comparison | Column–literal: the literal is normalized **to the column's scale at compile time**, and digits beyond `s` are a positioned statement error — rounding a literal to make it match is a silent wrong answer. Column–column (a join residual): **same `(p, s)` only**; differing scales answer `Unsupported`. Rescaling under overflow semantics is deferred whole, not half-shipped |
| TY7 | Validation | `EncodeOneValue` is the **only gate**: it parses `YYYY-MM-DD`, `YYYY-MM-DD HH:MM:SS[.ffffff]` and decimal strings, rejects out-of-range and malformed input as a positioned statement error, and range-checks against the type's width. Decode never re-validates — stored bytes were proven at the gate, same principle the codec already runs on |
| TY8 | Value functions | `NOW()`, `CURRENT_DATE`, arithmetic on dates: **not in v1**. A value function imports an evaluation-time question (once per statement? per row?) that "the query is the plan" has no slot for. Clients send literals |
| TY9 | Catalog & migration | Purely additive `type_val`s: `kTypeValDate = 11`, `kTypeValTimestamp = 12`; DECIMAL reuses the reserved `kTypeValDecimal = 7`. No existing relation changes meaning. `(p, s)` persists **packed into `SysColumnRow::len`** — **`[CONFIRMED 2026-08-07]`, see §7a**: no format change, no version bump, every pre-existing data file still mounts |

---

## 1. What a type is here `[CONFIRMED]`

A type in this engine is four things and nothing else: a **width** (a schema
constant, invariant 13), an **encoding** (`EncodeOneValue`), a **decoding**
(`DecodeOneValueInto`), and a **comparison** (`CompareValues`, dispatched on
`type_val`). Everything downstream — btree clustering, GROUP BY key
encoding, Cabin key matching, the probe memo, trail replay — consumes those
four and needs no per-type knowledge. The three new types are designed to
keep it that way:

| type | width | on-disk | compares as |
|---|---|---|---|
| `DATE` | 4 | int32 LE, epoch days | signed int |
| `TIMESTAMP` | 8 | int64 LE, UTC micros | signed int |
| `DECIMAL(p,s)` | 8 | int64 LE, unscaled | signed int (same-scale, TY6) |

All three compare as signed integers, which means **every ordered structure
in the engine works on them unmodified**: a BTREE clustered on nothing new,
a `kRange`'s bounds, MIN/MAX through the existing int arm, first-seen group
keys encoding the int as they encode any int. This is not a coincidence; it
is the selection criterion TY2 and TY4 applied.

## 2. Grammar & DDL `[CONFIRMED]`

```
type ::= ... existing ... | DATE | TIMESTAMP | DECIMAL ( int , int )
```

`DATE`/`TIMESTAMP`/`DECIMAL` are type names in the DDL position only —
unreserved, like every keyword this parser matches, so columns named `date`
remain legal. `DECIMAL(p, s)` requires both arguments (no `DECIMAL`, no
`DECIMAL(p)` — a default scale is a silent decision about someone's money);
`p` and `s` outside TY2's bounds are positioned errors at CREATE TABLE.

Value positions take **string literals** (TY3). An unquoted `12.34` remains
what it is today — a parse error — and the error message gains a hint:
`decimal values are written as strings: '12.34'`.

## 3. Semantics `[CONFIRMED]`

### 3.1 Literal coercion is a compile-time act

A predicate `WHERE price = '12.34'` against a `DECIMAL(10,2)` column
compiles to a comparison whose right side is **already the scaled integer
1234** — the string is parsed once, at compile, by the same routine
`EncodeOneValue` uses (one parser, two callers, zero drift). Per-row
evaluation is then an int64 comparison, which keeps the residual path on
the cost profile `bench/results-scenario1-vs-pg.md`'s attribution demands
and makes the raw-byte residual optimization (its F3) apply to these types
for free. The same holds for `'2026-08-06'` against a `DATE` column. A
literal that does not parse as the column's type is a positioned error at
compile, not a row-by-row false.

### 3.2 DECIMAL arithmetic

There is none in v1 — no `+`, no `*`, no expressions (the grammar has
none). What exists is comparison (TY6) and aggregation: `SUM` over
`DECIMAL(p,s)` folds unscaled int64 through the checked adder and yields
`DECIMAL(18,s)` semantics — overflow is a statement error exactly as AG3
states, and the answer's scale is the column's. `MIN`/`MAX` are int
comparisons. `SUM` over `DATE`/`TIMESTAMP` is refused (`InvalidArgument`) —
a sum of dates is a statement nobody meant; `MIN`/`MAX` over them are exact
and useful. `COUNT` is type-blind as always.

**AVG remains `Unsupported` and this spec deliberately does not lift it.**
The stated reason in `docs/aggregate.md` — no decimal kind — is now false,
so the refusal message drops that clause; but AVG's return scale, its
rounding rule, and divide-semantics are one decision (`aggregate.md` §10
gains the item), and deciding them as a side effect of a types spec is how
two documents come to disagree.

### 3.3 Rendering happens at the boundary

`FormatValue` gains the column's `type_val` (signature
`FormatValue(std::uint32_t type_val, const AstValue&)`, with `0` preserving
today's behavior for every existing caller). A `DATE` renders
`2026-08-06`, a `TIMESTAMP` renders `2026-08-06 09:15:00.250000` (always
six fractional digits when non-zero, none when zero `[PROPOSED]`), a
`DECIMAL(p,s)` renders with exactly `s` fractional digits (`12.30`, not
`12.3` — the scale is part of the value's meaning). **Decode does not
format**: a date's `AstValue` is its integer, `raw_int_text` stays empty,
and the string exists only for rows actually emitted — the same
per-row-string discipline the int decoder documents, and the reason TY5
chose kInt reuse over a rendered representation.

### 3.4 NULLs, keys, and the rest

NULL handling is untouched: a NULL date is a NULL like any other, skipped
by aggregates, one group under GROUP BY. The Keystone pk remains uint64 —
none of the new types can be column 0. Cabin keys, probe keys, and
Waystone trails treat the new types as the integers they are; no trust-
model text changes.

## 4. What changes where `[CONFIRMED]`

Small, named, and closed:

- `well_known.hpp`: `kTypeValDate`, `kTypeValTimestamp`; width table
  entries; `IsIntegerTypeVal` **unchanged** (a date is not an integer type
  to SUM-type-checking, deliberately — that is what makes §3.2's SUM
  refusal a one-line check).
- `SysColumnRow`: `(p, s)` persistence per TY9.
- Parser DDL: the three type productions; positioned bound checks.
- `EncodeOneValue` / `DecodeOneValueInto`: three arms each, plus the shared
  text parsers (`ParseDateLiteral`, `ParseTimestampLiteral`,
  `ParseDecimalLiteral`) used by encode and by compile-time coercion.
- `AstValue`: `kDecimal` kind (unscaled int64 + `std::uint8_t scale`).
- `CompareValues`: `kTypeValDecimal/Date/Timestamp` dispatch to the int
  arm; a kDecimal↔kDecimal comparison asserts equal scales (TY6 proved it
  at compile).
- Step compiler: literal coercion (§3.1); TY6's mixed-scale refusals; SUM
  argument rules (§3.2).
- `FormatValue`: the `type_val` parameter (§3.3) and its three renderers.
- Docs: `client-manual.md` §3's refusal text; `sys.columns` exposure.

Not changed, stated so a diff can be checked against it: the tuple layout
rules, the WAL and undo formats (rows stay fixed-size), the step VM, the
Waystone and Cabin trust models, `kFingerprintVersion`, and every existing
`type_val`'s meaning.

## 4a. TY9 settled: `(p, s)` rides in `len` `[CONFIRMED 2026-08-07]`

TY9 left this gated: a spare `SysColumnRow` field if one exists, otherwise a
catalog format change behind a bootstrap version bump. Workplan TY02 was
told to flag before deciding. It flagged, and the answer is better than
either branch anticipated.

**There is no *reserved* field** — `SysColumnRow` is exactly packed, and
`kOnDiskSize` is the sum of its members. But `len` is **dead weight for
every type but two**: `RowLayout::ColumnWidth` reads it only for `char`, and
derives every other width from `type_val` alone. Its remaining readers were
display-only.

So `(p, s)` — two values bounded by 18 — pack into `len`'s low sixteen bits,
precision high and scale low, with explicit shift/mask helpers
(`PackDecimalLen`, `DecimalPrecisionOf`, `DecimalScaleOf` in
`catalog/rows.hpp`; invariant 6 forbids a compiler bitfield for a persisted
format). Sixteen bits stay zero and available.

**What this bought:** no superblock version bump, so no pre-existing data
file stops mounting. The last four bootstrap-relation additions each cost
exactly that, and the fkey one is the most recent.

**What it cost, stated so nobody has to rediscover it:** `len` is no longer
readable as "a width" without knowing the column's type. Two paths read it
that way — `sys.columns` and `DESCRIBE` — and both now render the *declared
type* instead (`decimal(10,2)`, `char(8)`, `date`) through one function,
`ColumnTypeText`, so they cannot come to disagree. `sys.columns`'s `len`
column is replaced by `type`; `DESCRIBE` drops `len=` and its `type=` now
carries the parameters. Both are client-visible surface changes and are the
whole price.

## 5. What v1 is not

`FLOAT64` (TY1) · bare numeric literals `12.34` (TY3, phase 2) · time
zones and `timestamptz` (TY4) · `p > 18` (TY2, future int128 type) ·
cross-scale DECIMAL comparison and rescaling (TY6) · date/decimal
arithmetic and value functions (TY8) · `AVG` (§3.2 — unlocked in
precondition, deliberately not decided here) · casts between the new types
and anything (`'2026-08-06'` into a varchar column stays a plain string).

## 6. Contract tests — done when

1. **Round trip**: for each type, encode → decode → format reproduces the
   literal exactly, across the range edges (`0001-01-01`? — no: the valid
   `DATE` range is **1900-01-01 .. 2999-12-31** and `TIMESTAMP` its
   microsecond equivalent `[PROPOSED]`; outside is a positioned encode
   error, and the round-trip corpus pins both edges).
2. **Ordering**: btree clustering, `kRange` bounds, MIN/MAX and ORDER-less
   scans agree with integer order on all three types; `DECIMAL('12.30')`
   equals `DECIMAL('12.3')` at scale 2.
3. **Coercion errors**: `'12.345'` into `DECIMAL(10,2)`, `'2026-02-30'`,
   `'not a date'`, `p`/`s` bound violations — each a positioned error, at
   compile for predicates and at encode for INSERT.
4. **Mixed-scale join residual** answers `Unsupported` with position.
5. **Aggregates**: `SUM` over DECIMAL exact at scale, overflow at the
   int64 edge is a statement error; `SUM` over DATE refused; `MIN`/`MAX`
   over all three exact; GROUP BY on a DATE key groups and emits
   first-seen.
6. **Fingerprint invariance**: the golden corpus, pre-existing statements
   only, hashes identically; a new statement with a date literal hashes as
   a string-literal statement.
7. **Rendering**: §3.3's formats pinned, including trailing-zero scale and
   the `type_val = 0` compatibility of every existing `FormatValue` caller.
8. **Catalog**: `(p, s)` survives restart; a pre-types data file opens and
   serves unchanged (or, if TY9 forced a format bump, refuses with the
   version message — whichever TY02 decided, pinned).

## 7. Open items — do not assume

- `[PROPOSED]` valid ranges in §6.1 and the timestamp rendering rule in
  §3.3 — ratify with the first client that cares.
- AVG's return type, scale and rounding (`aggregate.md` §10).
- Phase 2: bare numeric literals (TY3) and its fingerprint analysis.
- int128 `DECIMAL` for `p > 18` (TY2).
