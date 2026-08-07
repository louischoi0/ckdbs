# Types — Workplan

Work instructions, companion to `docs/types.md` (TY1–TY9). Tasks
`TY01`–`TY09`.

Execution rules:
- Do tasks in numeric order unless "needs" says otherwise.
- Each task ships with its listed tests in the same change.
- If a task touches an `[OPEN]` or `[PROPOSED]` item — build the proposed
  default, do not decide differently, and flag it. TY02 is the one task
  that may have to *stop and flag before building* (catalog format).
- The lexer is off-limits (TY3). A diff touching `lexer.cpp` or
  `token.hpp` in service of this workplan means the literal decision was
  violated, not that the task needed it. Same falsifiability style as the
  aggregate workplan's step-VM rule.
- The round-trip suite (`TY08`) is regression-mandatory from the moment it
  exists.

---

## TY01 — Widths, type_vals, and the shared text parsers

`kTypeValDate = 11`, `kTypeValTimestamp = 12` (`[PROPOSED]` numbers — take
the next free ones if these are taken); width-table entries (4, 8, 8 with
`kTypeValDecimal`); `IsIntegerTypeVal` untouched. The three literal
parsers (`ParseDateLiteral`, `ParseTimestampLiteral`,
`ParseDecimalLiteral(p, s)`) as free functions with positioned errors —
**one parser per type, called from both the encoder and the compiler**, so
coercion and storage cannot drift.

*Done when:* parser unit tests pin the accepted grammar, the §6.1
`[PROPOSED]` range edges, leap years, `'2026-02-30'`-class rejections, and
scale-overflow (`'12.345'` at s=2) — all with positions.

## TY02 — Catalog: persisting `(p, s)` (needs TY01) — **DONE**

Find where a column's `(p, s)` lives. **First** check `SysColumnRow` for a
reserved/spare field wide enough for two uint8s; if one exists, pack there
and document the packing. **If none exists, stop and flag** — widening the
catalog row is a data-file format change, and the choice between a
bootstrap version bump and a spare-field retrofit is TY9's explicitly
gated decision, not this task's.

*Done when:* `(p, s)` survives a restart; `sys.columns` exposes the type
faithfully (`decimal(10,2)`, `date`); a pre-types data file behaves per
whichever gate was decided — and the flag, if raised, is in the spec
before the code lands.

*Outcome:* the flag was raised and answered — **`len`**, which
`RowLayout::ColumnWidth` reads only for `char` and which was dead weight
for every other type. Packed precision-high/scale-low with explicit
shift/mask helpers (invariant 6). **No format change and no version bump**,
where widening the row would have stopped every pre-existing data file from
mounting. Recorded in spec §4a before the code landed, as this task
required. The price is that `len` is no longer readable as a width without
the type, so `sys.columns` and `DESCRIBE` render the declared type through
one shared `ColumnTypeText` instead.

## TY03 — DDL (needs TY01, TY02)

The three type productions in CREATE TABLE, `DECIMAL(p, s)` with mandatory
both-arguments and TY2's bounds, positioned errors, `client-manual.md` §3
updated (decimal's refusal text replaced by its acceptance; float's
refusal kept and now citing TY1).

*Done when:* CREATE TABLE round-trips through the catalog for all three;
`DECIMAL`, `DECIMAL(19,0)`, `DECIMAL(5,6)` are positioned errors; a column
named `date` still works everywhere it did.

## TY04 — Codec (needs TY01, TY02)

`EncodeOneValue`: three arms, TY7's single-gate validation, range checks.
`DecodeOneValueInto`: three arms — **int-only, no formatting, no strings**
(`raw_int_text` stays empty; the per-row string cost the int decoder's
comment documents is a rule here, not an accident). `kDecimal` in
`AstValue` (unscaled int64 + scale).

*Done when:* encode→decode round-trips bit-exactly across the range
corpus; an allocation counter shows decode of the new types allocates
nothing; invariant-13 checks still hold (row size unchanged by NULLs,
padding zero-filled).

## TY05 — Comparison & compile-time coercion (needs TY04)

`CompareValues` dispatch for the three type_vals (int arm); the step
compiler coerces string literals against typed columns via TY01's parsers
(§3.1), refuses mixed-scale column–column residuals (`Unsupported`,
positioned), and type-checks SUM per §3.2.

*Done when:* predicate tests cover `=`/`<`/`BETWEEN`-shape residuals on
all three types against string literals; the compiled predicate's rhs is
the scaled/epoch integer (asserted via plan or spec inspection, not
inferred); mixed-scale refusal is pinned; a coercion failure names the
byte of the literal.

## TY06 — Rendering (needs TY04)

`FormatValue(type_val, value)` per §3.3, `0` preserving every existing
call site (mechanical sweep, no behavior change for existing types).

*Done when:* §6.7's pinned formats pass; the sweep is provably complete
(no remaining single-argument caller); scale-faithful trailing zeros
(`12.30`) and the timestamp fractional rule are pinned as `[PROPOSED]`.

## TY07 — End to end through the dispatcher (needs TY03–TY06)

INSERT / SELECT / UPDATE / JOIN / GROUP BY / Cabin / Waystone over the new
types, wired and tested at the dispatcher level. Nothing here should need
new engine code — this task exists to prove §1's claim that the four
type primitives are all anything consumes.

*Done when:* an aggregated date-key statement groups first-seen; a Cabin
on a DATE column records, serves and drops per its contract test pattern;
a trail over a decimal-residual probe chain replays; SUM-over-DATE and
mixed-scale refusals surface through the wire with positions.

## TY08 — Contract suite (needs TY01–TY07)

`tests/types_contract_test.cpp` collecting spec §6 items 1–8, regression-
mandatory, byte-for-byte comparisons in the established contract-test
style.

*Done when:* all eight pinned and in the default test target.

## TY09 — Docs & spec closure (needs TY08)

`docs/aggregate.md`: AVG's refusal message drops the "no decimal kind"
clause and §10 gains the AVG-return-type item (spec §3.2). `docs/types.md`:
ratify or amend the `[PROPOSED]` ranges/renderings from whatever TY07's
tests forced, with the change cited.

*Done when:* the two specs agree with the code and with each other; grep
for the old refusal text finds nothing.
