# KDS Types — DATE, TIMESTAMP, DECIMAL (Specification)

**Status:** Official specification, decisions confirmed 2026-08-06 (TY1–TY9, §0).
Lifts the CREATE TABLE refusal of `decimal` (`client-manual.md` §3) for the
fixed-width form specified here; `float` stays refused. Companion tasks:
`docs/workplan-types.md`. Markers: `[CONFIRMED]`, `[PROPOSED]`, `[OPEN]`.
Consistent with `docs/rules/rule-fixed-length-tuple.md` (invariant 13),
`docs/spec/parser-v2.md`, `docs/spec/aggregate.md` (AG-series),
`docs/spec/heap-and-tuple.md`, `docs/spec/cabin.md`.

*(Both filenames on the first two lines were wrong until TY09: this file
cited `docs/types-workplan.md` for its own workplan, which is
`docs/workplan-types.md`, and `docs/aggregate.md`, which has never
existed. `docs/workplan-aggregate.md` records the same slip from the
other side.)*

## 0. Decision Record `[CONFIRMED 2026-08-06]`

| # | Decision | Choice |
|---|---|---|
| TY1 | v1 types | **`DATE`, `TIMESTAMP`, `DECIMAL(p,s)`** — all fixed-width, so invariant 13 is untouched. `FLOAT64` stays out (`Unsupported` at CREATE TABLE, reserved `kTypeValFloat = 6` unchanged): IEEE comparison and aggregation semantics conflict with the engine's exactness discipline, and nothing in it is needed to unblock the workloads that asked. `TIME`, `INTERVAL`, `timestamptz`: not reserved, not parsed |
| TY2 | DECIMAL representation | **Scaled int64**: the unscaled value in 8 bytes, `1 ≤ p ≤ 18`, `0 ≤ s ≤ p`. Comparison, grouping and SUM reuse the checked-int64 machinery verbatim (AG3). A variable-width numeric violates invariant 13 by construction; `p > 18` is a *separate type* (int128, 16 bytes — a different schema constant, so the two coexist), never a widening of this one — **and that type is built (2026-08-07, §2a)**: `decimal(p, s)` with `19 ≤ p ≤ 38` selects `kTypeValDecimalWide` at the one DDL site, also declarable as `decimal128(p, s)` |
| TY3 | Literals | **Quoted string literals only** in v1: `'2026-08-06'`, `'12.34'`. The column's `type_val` decides the interpretation. **The lexer does not change** — no decimal-point token, no date token — so every previously-accepted statement lexes identically and the fingerprint argument is structural: `kFingerprintVersion` unmoved. ~~A bare `12.34` numeric token is phase 2, gated on its own fingerprint analysis~~ — **phase 2 built 2026-08-07**: the bare form is sugar for the quoted string of its spelling, the gating analysis is in §2 and `src/parser/fingerprint.cpp`, and the version still did not move |
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

Value positions take **string literals** (TY3), and — since phase 2,
**built 2026-08-07** — a bare `12.34` as well. The phase-2 rule is one
sentence: **a bare numeric literal is the quoted string of its spelling,
exactly** — the lexer fuses `digits . digits` (both sides mandatory; `12.`
and `.5` stay the errors they were) into one token, the parser produces
the AstValue `'12.34'` would, and the fingerprint hashes the same argument
bytes, so the two spellings are one statement everywhere: one pattern_id,
one arg_hash, one meaning, one set of errors. No new `ValueType`, no new
coercion path, no carve-outs — a bare `1.5` into a varchar column stores
the string `1.5`, as the quoted form always did. The v1 line promised the
parse error "gains a hint" naming the quoted form; the hint was never
built, and acceptance retired the message it would have decorated.

The fingerprint analysis phase 2 was gated on, in brief (in full:
`src/parser/fingerprint.cpp` at kNumLit, pinned by `fingerprint_test.cpp`
and the golden corpus): fusing the tokens moves the hash only of
statements containing digit-dot-digit, which lexed but parsed in no
production — fingerprintable yet unrecordable, so no stored `pattern_id`
moves and **`kFingerprintVersion` stays 1**. Every pre-existing golden
corpus line passes unchanged as the witness.

## 2a. The wide decimal `[CONFIRMED 2026-08-07]`

TY2's separate type, built. **A type is still four things**: width 16
(int128, two LE uint64 halves via explicit helpers — invariant 6, never a
memcpy of the builtin), the shared digit-walk parser at a 38-digit cap
(`10^38 − 1 < 2^127`; one template body serves both widths, TY01's
one-parser rule surviving the split), an int128 comparison behind the same
equal-kind/equal-scale contract, and a hand-peeled rendering. Everything
else is the narrow type's machinery observed to hold: coercion through
`CoerceLiteralToColumn`, grouping/DISTINCT under a tag of its own,
`SUM`/`AVG` through an int128 accumulator beside the int64 one (which is
a product contract and does not widen), the Cabin key, and the wire's 16
LE bytes with `(p, s)` in `type_mod` — the width §6's DECIMAL decision
reserved.

Four rules. **The declared precision selects the width at the one DDL
site**: `decimal(p ≤ 18, s)` is the 8-byte type, `decimal(19 ≤ p ≤ 38, s)`
the 16-byte one, and `decimal128(p, s)` names the wide type directly with
bounds exclusive of the narrow ones — one declaration selects exactly one
type, and DESCRIBE renders the type a column *got* (`decimal128(24,6)`).
**Cross-width comparison is refused at compile** like cross-scale, and
must be: at run time the pair is a kind mismatch answering false per row —
zero rows wearing a right answer's shape. **`kDecimalWide` is a
`ValueType` of its own**, not a width flag on `kDecimal`, so every
consumer that reads `int_val` was surfaced by the compiler instead of
silently truncating; the value is `Int128FromHalves(dec_hi, int_val)`.
And **an integer literal that wrapped int64 is refused wherever `int_val`
is the value** — building this type surfaced that `= 36893488147419103232`
against a *narrow* decimal coerced as the 0 it wrapped to and matched
0.00; the wide arm reads the preserved digit text, and date, timestamp
and narrow-decimal coercion now refuse a wrapped literal outright.
Purely additive: no format change, no version bump, one new `sys.types`
row (`decimal128`, oid 33, type_val 13).

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

**AVG folds since 2026-08-07** — decided in `docs/spec/aggregate.md` §3.4,
the document this spec deliberately handed the item to rather than
settling it in passing: the answer is at the argument column's declared
scale, rounded half-even on the exact integer pair, and a column that
declared no scale (the integer types) is refused at compile. The handoff
worked as designed — the decision lives in one document and this one only
points at it.

*Closed at TY09, historical note.* The correct file is
`docs/spec/aggregate.md`, not `docs/aggregate.md`, which does not exist
and never did — this paragraph and the workplan both cited it, which is
the kind of reference that survives precisely because nobody follows it.
(TY09 also scrubbed the refusal's stale "no decimal kind" clause from two
code comments, `aggregate.md` and `CLAUDE.md`; the refusal itself,
and its "compute it from SUM and COUNT" message, are gone entirely now
that AVG folds — only a non-decimal argument still gets an error, the
compile-time one above.)

### 3.3 Rendering happens at the boundary

`FormatValue` gains the column's `type_val` (signature
`FormatValue(std::uint32_t type_val, const AstValue&)`, with `0` preserving
today's behavior for every existing caller). A `DATE` renders
`2026-08-06`, a `TIMESTAMP` renders `2026-08-06 09:15:00.250000` (always
six fractional digits when non-zero, none when zero — **`[CONFIRMED
2026-08-07]`**, TY09; pinned by `tests/types_contract_test.cpp` item 7,
and by the round-trip corpus, which cannot reproduce a literal unless the
rule is stable in both directions), a
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

`FLOAT64` (TY1) · ~~bare numeric literals `12.34` (TY3, phase 2)~~ —
**built 2026-08-07**, see §2 · time
zones and `timestamptz` (TY4) · ~~`p > 18` (TY2, future int128 type)~~ —
**built 2026-08-07**, see §2a ·
cross-scale DECIMAL comparison and rescaling (TY6) · date/decimal
arithmetic and value functions (TY8) · `AVG` (§3.2 — unlocked in
precondition, deliberately not decided here) · casts between the new types
and anything (`'2026-08-06'` into a varchar column stays a plain string).

## 6. Contract tests — done when

1. **Round trip**: for each type, encode → decode → format reproduces the
   literal exactly, across the range edges (`0001-01-01`? — no: the valid
   `DATE` range is **1900-01-01 .. 2999-12-31** and `TIMESTAMP` its
   microsecond equivalent — **`[CONFIRMED 2026-08-07]`**, TY09; outside is
   a positioned encode error, and the round-trip corpus pins both edges
   plus the two rejections just outside them).
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

- ~~`[PROPOSED]` valid ranges in §6.1 and the timestamp rendering rule in
  §3.3~~ — **ratified 2026-08-07 (TY09)**, not by a client but by the
  contract suite: `tests/types_contract_test.cpp` pins both range edges,
  the two rejections just outside them, and the fractional-digit rule, so
  these numbers are load-bearing now and moving one is a decision with a
  failing test attached rather than an adjustment. Widening the `DATE`
  range stays cheap — the encoding is a signed epoch day with room to
  spare; narrowing it is data-losing and needs a migration story.
- ~~**AVG's return type, scale and rounding**~~ — **decided and built
  2026-08-07** in `docs/spec/aggregate.md` §3.4, the document TY09 handed
  the item to: declared scale, half-even, integer columns refused. §3.2
  above now points at it.
- ~~Phase 2: bare numeric literals (TY3) and its fingerprint analysis~~ —
  **built 2026-08-07 (TY10)**. The analysis it was gated on concluded no
  version bump: the fused token sequence appeared only in statements no
  production parsed, so the moved hashes were never storable. §2 carries
  the rule; `docs/workplan-types.md` TY10 the outcome. What phase 2 still
  is not: scientific notation (`1e5` lexes as it always did — an integer
  and an identifier) and a leading-dot form (`.5`), both refused rather
  than guessed at.
- ~~int128 `DECIMAL` for `p > 18` (TY2)~~ — **built 2026-08-07 (§2a)**:
  `decimal(19 ≤ p ≤ 38, s)` / `decimal128(p, s)`, type_val 13, 16 bytes,
  everything the narrow type does including SUM/AVG and the wire. What
  remains out: `p > 38` (no representation), cross-width comparison
  (refused with the cross-scale rule), and the narrow type's own contracts
  are untouched — its int64 accumulator did not widen.
