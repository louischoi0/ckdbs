# KDS Types — DATE, TIMESTAMP, DECIMAL (Specification)

**Status:** Official specification. `decimal` in its fixed-width forms,
`date`, `timestamp`, `char(N)` and `varchar(N)` are declarable; `float`
is refused at CREATE TABLE. Consistent with
`docs/rules/rule-fixed-length-tuple.md` (invariant 13),
`docs/spec/parser-v2.md`, `docs/spec/aggregate.md`,
`docs/spec/heap-and-tuple.md`, `docs/spec/cabin.md`.

## 0. Decision Record `[CONFIRMED]`

| # | Decision | Choice |
|---|---|---|
| TY1 | Types | **`DATE`, `TIMESTAMP`, `DECIMAL(p,s)`** — all fixed-width, so invariant 13 is untouched. `FLOAT64` is out (`Unsupported` at CREATE TABLE, reserved `kTypeValFloat = 6` unchanged): IEEE comparison and aggregation semantics conflict with the engine's exactness discipline. `TIME`, `INTERVAL`, `timestamptz`: not reserved, not parsed |
| TY2 | DECIMAL representation | **Scaled int64**: the unscaled value in 8 bytes, `1 ≤ p ≤ 18`, `0 ≤ s ≤ p`. Comparison, grouping and SUM reuse the checked-int64 machinery verbatim (`aggregate.md` AG3). A variable-width numeric violates invariant 13 by construction; `p > 18` is a *separate type* (int128, 16 bytes — a different schema constant, so the two coexist), never a widening of this one: `decimal(p, s)` with `19 ≤ p ≤ 38` selects `kTypeValDecimalWide` at the one DDL site, also declarable as `decimal128(p, s)` (§2a) |
| TY3 | Literals | **Quoted string literals** — `'2026-08-06'`, `'12.34'` — and a bare `12.34`, which is sugar for the quoted string of its spelling (§2). The column's `type_val` decides the interpretation. There is no decimal-point token and no date token beyond the fused `digits . digits`, and `kFingerprintVersion` did not move for either form |
| TY4 | Time encoding | `DATE` = **days since 1970-01-01, int32** (4 bytes). `TIMESTAMP` = **microseconds since the epoch, UTC, int64** (8 bytes). **Storage is always UTC**; there is no session time zone, no conversion, no `timestamptz` — rendering what UTC means locally is the client's act, and this is a documented product constraint, not a gap |
| TY5 | Value model | `DATE`/`TIMESTAMP` **reuse `AstValue` kInt** — the value *is* an integer; only its rendering differs, and rendering happens at the emission boundary (§3.3), not in the value. **`DECIMAL` alone adds a kind**: `kDecimal` carrying the unscaled int64 plus its scale (and `kDecimalWide` for the 16-byte type, §2a). One new kind per representation, not three, is what keeps every switch over `ValueType` from growing arms that behave identically |
| TY6 | Mixed-scale comparison | Column–literal: the literal is normalized **to the column's scale at compile time**, and digits beyond `s` are a positioned statement error — rounding a literal to make it match is a silent wrong answer. Column–column (a join residual): **same `(p, s)` only**; differing scales answer `Unsupported`. Rescaling under overflow semantics is not offered |
| TY7 | Validation | `EncodeOneValue` is the **only gate**: it parses `YYYY-MM-DD`, `YYYY-MM-DD HH:MM:SS[.ffffff]` and decimal strings, rejects out-of-range and malformed input as a positioned statement error, and range-checks against the type's width. The valid `DATE` range is **1900-01-01 .. 2999-12-31** and `TIMESTAMP`'s is its microsecond equivalent; outside is a positioned encode error. Decode never re-validates — stored bytes were proven at the gate, same principle the codec already runs on |
| TY8 | Value functions | `NOW()`, `CURRENT_DATE`, arithmetic on dates: **none**. A value function imports an evaluation-time question (once per statement? per row?) that "the query is the plan" has no slot for. Clients send literals |
| TY9 | Catalog | Purely additive `type_val`s: `kTypeValDate = 11`, `kTypeValTimestamp = 12`, `kTypeValDecimalWide = 13`; DECIMAL reuses the reserved `kTypeValDecimal = 7`. No existing relation changes meaning. `(p, s)` persists **packed into `SysColumnRow::len`** (§4a): no format change, no version bump |

---

## 1. What a type is here `[CONFIRMED]`

A type in this engine is four things and nothing else: a **width** (a schema
constant, invariant 13), an **encoding** (`EncodeOneValue`), a **decoding**
(`DecodeOneValueInto`), and a **comparison** (`CompareValues`, dispatched on
`type_val`). Everything downstream — btree clustering, GROUP BY key
encoding, Cabin key matching, the probe memo, trail replay — consumes those
four and needs no per-type knowledge. The types here are designed to keep
it that way:

| type | width | on-disk | compares as |
|---|---|---|---|
| `DATE` | 4 | int32 LE, epoch days | signed int |
| `TIMESTAMP` | 8 | int64 LE, UTC micros | signed int |
| `DECIMAL(p,s)`, `p ≤ 18` | 8 | int64 LE, unscaled | signed int (same-scale, TY6) |
| `DECIMAL(p,s)`, `19 ≤ p ≤ 38` / `DECIMAL128(p,s)` | 16 | int128 as two LE uint64 halves | signed int128 (same-scale, same-width, §2a) |

All compare as signed integers, which means **every ordered structure in
the engine works on them unmodified**: a BTREE clustered on nothing new, a
`kRange`'s bounds, MIN/MAX through the existing int arm, first-seen group
keys encoding the int as they encode any int. This is not a coincidence; it
is the selection criterion TY2 and TY4 applied.

## 2. Grammar & DDL `[CONFIRMED]`

```
type ::= ... existing ... | DATE | TIMESTAMP | DECIMAL ( int , int ) | DECIMAL128 ( int , int )
```

`DATE`/`TIMESTAMP`/`DECIMAL`/`DECIMAL128` are type names in the DDL
position only — unreserved, like every keyword this parser matches, so
columns named `date` remain legal. `DECIMAL(p, s)` requires both arguments
(no `DECIMAL`, no `DECIMAL(p)` — a default scale is a silent decision about
someone's money); `p` and `s` outside TY2's bounds are positioned errors at
CREATE TABLE.

Value positions take **string literals** (TY3) and a bare `12.34`. The
rule is one sentence: **a bare numeric literal is the quoted string of its
spelling, exactly** — the lexer fuses `digits . digits` (both sides
mandatory; `12.` and `.5` are errors) into one token, the parser produces
the AstValue `'12.34'` would, and the fingerprint hashes the same argument
bytes, so the two spellings are one statement everywhere: one pattern_id,
one arg_hash, one meaning, one set of errors. No new `ValueType`, no new
coercion path, no carve-outs — a bare `1.5` into a varchar column stores
the string `1.5`, as the quoted form always did. There is no scientific
notation: `1e5` lexes as an integer and an identifier.

Fusing the token moved the hash only of statements containing
digit-dot-digit, which lexed but parsed in no production — fingerprintable
yet unrecordable — so no stored `pattern_id` moved and `kFingerprintVersion`
did not move (`src/parser/fingerprint.cpp` at kNumLit, pinned by the golden
corpus).

## 2b. `char(N)` and `varchar(N)` `[CONFIRMED]`

```
type ::= ... | CHAR [ ( int ) ] | VARCHAR [ ( int ) ]
```

Both take an **optional** single argument, which is why the grammar
brackets them and `DECIMAL`'s are not bracketed — a bare `decimal` is
refused because a default scale decides what a value *means*, and neither
default here decides anything.

**`char(N)` — a fixed cell of N bytes.** `char` alone is `char(N=1)`, the
standard's own default. Values are zero-padded to `N` and read back to the
first NUL; a longer value is refused naming `N`, and a value *containing*
a NUL is refused, because the type could not read it back as written.
Comparison is byte-wise over the unpadded value, so `'ab' <> 'ab '` — there
is no `PAD SPACE` collation, a divergence the manual states.

**`varchar(N)` — the tagged cell, with N as that column's width.** `N` is
this column's `kds.inline_cell_width`, in the same unit and validated by
the same `storage::CheckInlineCellWidth`, so `16 ≤ N ≤ 4096` and
`varchar(8)` is refused. **There is no second threshold and no second name
for one.** `N` is a width, not a length cap: a value longer than `N − 3`
spills to the var-heap exactly as it always has, and the only length
refusal remains 8144 bytes. A bare `varchar` takes the instance's width.
`ALTER … TYPE varchar(M)` is refused: changing a cell's width rewrites
every row (`docs/rules/rule-fixed-length-tuple.md` §4).

**Neither costs a format event**, which is the point of putting both in
`len` — §4a's argument, reused unchanged: a `char`'s `len` is its width,
and a `varchar`'s is 0 for "the instance's" or the declared `N`. Reading 0
that way is what lets an existing file mount byte-identical. `DESCRIBE`
renders `char(8)` and `varchar(32)`, and a bare `varchar` renders **no**
width, because the instance's number is not a property of the column and
must not be reported as one.

The full rules: `docs/rules/rule-fixed-length-tuple.md` §4.

## 2a. The wide decimal `[CONFIRMED]`

TY2's separate type. **A type is still four things**: width 16 (int128,
two LE uint64 halves via explicit helpers — invariant 6, never a memcpy of
the builtin), the shared digit-walk parser at a 38-digit cap
(`10^38 − 1 < 2^127`; one template body serves both widths), an int128
comparison behind the same equal-kind/equal-scale contract, and a
hand-peeled rendering. Everything else is the narrow type's machinery:
coercion through `CoerceLiteralToColumn`, grouping/DISTINCT under a tag of
its own, `SUM`/`AVG` through an int128 accumulator beside the int64 one
(which is a product contract and does not widen), the Cabin key, and the
wire's 16 LE bytes with `(p, s)` in `type_mod`.

Four rules. **The declared precision selects the width at the one DDL
site**: `decimal(p ≤ 18, s)` is the 8-byte type, `decimal(19 ≤ p ≤ 38, s)`
the 16-byte one, and `decimal128(p, s)` names the wide type directly with
bounds exclusive of the narrow ones — one declaration selects exactly one
type, and DESCRIBE renders the type a column *got* (`decimal128(24,6)`).
**Cross-width comparison is refused at compile** like cross-scale, and
must be: at run time the pair is a kind mismatch answering false per row —
zero rows wearing a right answer's shape. **`kDecimalWide` is a
`ValueType` of its own**, not a width flag on `kDecimal`, so every
consumer that reads `int_val` is surfaced by the compiler instead of
silently truncating; the value is `Int128FromHalves(dec_hi, int_val)`.
And **an integer literal that wrapped int64 is refused wherever `int_val`
is the value** — the wide arm reads the preserved digit text, and date,
timestamp and narrow-decimal coercion refuse a wrapped literal outright
rather than coercing the value it wrapped to. Purely additive: no format
change, no version bump, one `sys.types` row (`decimal128`, oid 33,
type_val 13). `p > 38` has no representation and is refused.

## 3. Semantics `[CONFIRMED]`

### 3.1 Literal coercion is a compile-time act

A predicate `WHERE price = '12.34'` against a `DECIMAL(10,2)` column
compiles to a comparison whose right side is **already the scaled integer
1234** — the string is parsed once, at compile, by the same routine
`EncodeOneValue` uses (one parser, two callers, zero drift). Per-row
evaluation is then an int64 comparison, which keeps the residual path on
the integer cost profile and lets the raw-byte residual optimization apply
to these types for free. The same holds for `'2026-08-06'` against a
`DATE` column. A literal that does not parse as the column's type is a
positioned error at compile, not a row-by-row false.

### 3.2 DECIMAL arithmetic

There is none — no `+`, no `*`, no expressions (the grammar has none).
What exists is comparison (TY6) and aggregation: `SUM` over `DECIMAL(p,s)`
folds unscaled int64 through the checked adder and yields `DECIMAL(18,s)`
semantics — overflow is a statement error exactly as AG3 states, and the
answer's scale is the column's. `MIN`/`MAX` are int comparisons. `SUM`
over `DATE`/`TIMESTAMP` is refused (`InvalidArgument`) — a sum of dates is
a statement nobody meant; `MIN`/`MAX` over them are exact and useful.
`COUNT` is type-blind as always.

**`AVG`** is decided in `docs/spec/aggregate.md` §3.4, which owns it: the
answer is at the argument column's declared scale, rounded half-even on
the exact integer pair, and a column that declared no scale (the integer
types) is refused at compile.

### 3.3 Rendering happens at the boundary

`FormatValue` takes the column's `type_val` (signature
`FormatValue(std::uint32_t type_val, const AstValue&)`, with `0` preserving
the untyped behaviour for every existing caller). A `DATE` renders
`2026-08-06`, a `TIMESTAMP` renders `2026-08-06 09:15:00.250000` — always
six fractional digits when non-zero, none when zero — and a
`DECIMAL(p,s)` renders with exactly `s` fractional digits (`12.30`, not
`12.3` — the scale is part of the value's meaning). **Decode does not
format**: a date's `AstValue` is its integer, `raw_int_text` stays empty,
and the string exists only for rows actually emitted — the same
per-row-string discipline the int decoder documents, and the reason TY5
chose kInt reuse over a rendered representation.

### 3.4 NULLs, keys, and the rest

A NULL date is a NULL like any other (`docs/spec/null.md`), skipped by
aggregates, one group under GROUP BY. The Keystone pk remains uint64 —
none of these types can be column 0. Cabin keys, probe keys, and Waystone
trails treat the types as the integers they are; no trust-model text
changes.

## 4. Where each piece lives `[CONFIRMED]`

- `well_known.hpp`: `kTypeValDate`, `kTypeValTimestamp`,
  `kTypeValDecimalWide`; width table entries; `IsIntegerTypeVal` excludes
  them (a date is not an integer type to SUM-type-checking, deliberately —
  that is what makes §3.2's SUM refusal a one-line check).
- `SysColumnRow`: `(p, s)` persistence per TY9 (§4a).
- Parser DDL: the type productions; positioned bound checks.
- `EncodeOneValue` / `DecodeOneValueInto`: one arm per type, plus the
  shared text parsers (`ParseDateLiteral`, `ParseTimestampLiteral`,
  `ParseDecimalLiteral`) used by encode and by compile-time coercion.
- `AstValue`: `kDecimal` (unscaled int64 + `std::uint8_t scale`) and
  `kDecimalWide`.
- `CompareValues`: `kTypeValDecimal/DecimalWide/Date/Timestamp` dispatch
  to the int arms; a decimal↔decimal comparison asserts equal scales (TY6
  proved it at compile).
- Step compiler: literal coercion (§3.1); TY6's mixed-scale and §2a's
  mixed-width refusals; SUM argument rules (§3.2).
- `FormatValue`: the `type_val` parameter (§3.3) and its renderers.

## 4a. `(p, s)` rides in `len` `[CONFIRMED]`

`SysColumnRow` is exactly packed — `kOnDiskSize` is the sum of its members
and there is no reserved field — but `len` carries a width for only some
types: `RowLayout::ColumnWidth` reads it for `char`, `varchar` and the
decimal pair, and derives every other width from `type_val` alone.

So `(p, s)` — two values bounded by 38 — pack into `len`'s low sixteen
bits, precision high and scale low, with explicit shift/mask helpers
(`PackDecimalLen`, `DecimalPrecisionOf`, `DecimalScaleOf` in
`catalog/rows.hpp`; invariant 6 forbids a compiler bitfield for a
persisted format). Sixteen bits stay zero and available. No superblock
version bump followed, so no pre-existing data file stopped mounting.

What it costs: `len` is not readable as "a width" without knowing the
column's type. The two paths that read it that way — `sys.columns` and
`DESCRIBE` — render the *declared type* instead (`decimal(10,2)`,
`char(8)`, `varchar(32)`, `date`) through one function, `ColumnTypeText`,
so they cannot come to disagree. `sys.columns` exposes `type` rather than
`len`; `DESCRIBE`'s `type=` carries the parameters.

## 5. What is refused

`FLOAT64` (TY1) · time zones and `timestamptz` (TY4) · cross-scale
DECIMAL comparison and rescaling (TY6) · cross-width DECIMAL comparison
(§2a) · `p > 38` (§2a) · date/decimal arithmetic and value functions (TY8)
· scientific notation and a leading-dot literal (§2) · casts between these
types and anything (`'2026-08-06'` into a varchar column stays a plain
string).

## 6. Contract tests

1. **Round trip**: for each type, encode → decode → format reproduces the
   literal exactly, across the range edges — TY7's `DATE` range
   `1900-01-01 .. 2999-12-31` and its `TIMESTAMP` equivalent — with the
   two rejections just outside them.
2. **Ordering**: btree clustering, `kRange` bounds, MIN/MAX and ORDER-less
   scans agree with integer order on all types; `DECIMAL('12.30')`
   equals `DECIMAL('12.3')` at scale 2.
3. **Coercion errors**: `'12.345'` into `DECIMAL(10,2)`, `'2026-02-30'`,
   `'not a date'`, `p`/`s` bound violations — each a positioned error, at
   compile for predicates and at encode for INSERT.
4. **Mixed-scale join residual** answers `Unsupported` with position.
5. **Aggregates**: `SUM` over DECIMAL exact at scale, overflow at the
   int64 edge is a statement error; `SUM` over DATE refused; `MIN`/`MAX`
   over all types exact; GROUP BY on a DATE key groups and emits
   first-seen.
6. **Fingerprint invariance**: the golden corpus, pre-existing statements
   only, hashes identically; a new statement with a date literal hashes as
   a string-literal statement.
7. **Rendering**: §3.3's formats pinned, including trailing-zero scale and
   the `type_val = 0` compatibility of every existing `FormatValue` caller.
8. **Catalog**: `(p, s)` survives restart; a pre-types data file opens and
   serves unchanged.

## 7. Open items — do not assume

Nothing is recorded here as open; §5's refusals are the current rules, and
`docs/rules/rule-fixed-length-tuple.md` §4 owns the `inline_cell_width`
default.
