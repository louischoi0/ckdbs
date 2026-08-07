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

## TY03 — DDL (needs TY01, TY02) — **DONE**

The three type productions in CREATE TABLE, `DECIMAL(p, s)` with mandatory
both-arguments and TY2's bounds, positioned errors, `client-manual.md` §3
updated (decimal's refusal text replaced by its acceptance; float's
refusal kept and now citing TY1).

*Done when:* CREATE TABLE round-trips through the catalog for all three;
`DECIMAL`, `DECIMAL(19,0)`, `DECIMAL(5,6)` are positioned errors; a column
named `date` still works everywhere it did.

*Note the state this ships in.* The three types are **declarable but not
yet storable** — the codec arms are TY04's — so an INSERT into one is
refused *by name*, citing TY04, rather than falling through to
"unrecognized type_val" and reporting a corrupt catalog for a column the
catalog is entirely right about. One test pins that message, so TY04 has to
remove it deliberately.

*Also:* type arguments are recognized by the **paren, not the type name**,
so a type that takes none refuses them in one place rather than each name
needing its own production — `int64(10, 2)` is an error rather than
silently ignored arguments.

## TY04 — Codec (needs TY01, TY02) — **DONE**

`EncodeOneValue`: three arms, TY7's single-gate validation, range checks.
`DecodeOneValueInto`: three arms — **int-only, no formatting, no strings**
(`raw_int_text` stays empty; the per-row string cost the int decoder's
comment documents is a rule here, not an accident). `kDecimal` in
`AstValue` (unscaled int64 + scale).

*Done when:* encode→decode round-trips bit-exactly across the range
corpus; an allocation counter shows decode of the new types allocates
nothing; invariant-13 checks still hold (row size unchanged by NULLs,
padding zero-filled).

*Outcome.* TY03's declarable-but-not-storable refusal is gone, and the
test that pinned it is now the test that stores. Four things to know.

**Encode accepts exactly two shapes per type and no third.** The literal a
client wrote, as a string, parsed and range-checked through TY01's
parsers; and the *decoded* form of a value already stored, which an UPDATE
carries back for every column its SET list did not touch. A third shape
would be a route for an unvalidated value to reach a page, which is what
TY7's single gate exists to prevent — so `AStoredValueSurvivesAnUpdateOf
AnotherColumn` and `ADecodedValueReEncodesUnchanged` pin the second shape
from both ends, at the dispatcher and at the codec.

**A decimal whose scale disagrees with its column is refused, not
rescaled** — rescaling would either drop digits or invent them, and TY6
defers cross-scale work whole rather than shipping half of it.

**`kDecimal` is the one `ValueType` this adds, and DATE/TIMESTAMP add
none**: a date *is* days since the epoch and a timestamp microseconds
since it, so both decode as `kInt` and differ only in rendering — which
happens at the emission boundary, and is why `SELECT` on a date column
currently shows `20672` rather than `2026-08-07`. That is TY06's
`FormatValue(type_val, value)` parameter, not a gap in this task. A
decimal renders correctly already, because it is the one kind carrying
enough to describe itself.

*Also:* the zero-allocation requirement needed a counter, and one already
existed inside `aggregate_test.cpp` — replacing the global `operator new`,
so a second copy would have been a duplicate symbol rather than a second
counter. It is `tests/alloc_counter.hpp` now, shared by both, with the
aggregate suite's existing `EXPECT_LT(without, withd)` serving as the
positive control that keeps the counter from silently measuring nothing.

## TY05 — Comparison & compile-time coercion (needs TY04) — **DONE**

`CompareValues` dispatch for the three type_vals (int arm); the step
compiler coerces string literals against typed columns via TY01's parsers
(§3.1), refuses mixed-scale column–column residuals (`Unsupported`,
positioned), and type-checks SUM per §3.2.

*Done when:* predicate tests cover `=`/`<`/`BETWEEN`-shape residuals on
all three types against string literals; the compiled predicate's rhs is
the scaled/epoch integer (asserted via plan or spec inspection, not
inferred); mixed-scale refusal is pinned; a coercion failure names the
byte of the literal.

*Outcome.* `tests/types_predicate_test.cpp` inspects the **compiled
chain**, not the rows that come back, because those are different
assertions: a statement that returned the right rows while re-parsing its
literal per row would pass an end-to-end test and fail this one, and §3.1
is a claim about cost as much as about meaning.

**One coercion helper, called from both lowering sites.** The SELECT
chain and the write filter build predicates separately, so a literal
meaning one thing in a `WHERE` and another in an `UPDATE`'s `WHERE` was a
live possibility rather than a hypothetical. `BETWEEN` is two chances to
forget in each of them, and the high bound is what a single-site fix
misses — pinned.

**An integer literal against a decimal column is scaled, through
`ParseDecimalLiteral`.** `amt = 12` means 12.00 and is exact, but scaling
it inline would be a second implementation of the precision and range
rules that parser already owns — the drift TY01's one-parser rule exists
to prevent. So the integer is rendered to text and handed to it: one
small string per predicate at *compile*, never per row.

**Column-column checks scale and nothing else.** Two `DECIMAL`s of
differing scale compare unscaled integers that mean different things, so
it is `Unsupported` at compile — including on a join's `ON`, which is the
same comparison. Broader cross-type checking was left alone; it is not
this task's, and no existing behaviour depended on it.

*Two things this needed that the task description did not name.*
`AstValue::byte_offset` was set for `$param` **only**, so "name the byte
of the literal" was unbuildable until the parser filled it in for every
literal — safe because nothing compares the field: chain identity renders
operand *values* (the aggregate contract suite's `RenderOperand`), Cabin
keys are built from kind and contents, and the fingerprint folds from
tokens. `kFingerprintVersion` did not move and the golden corpus passes
unchanged. And `AggregateItem` gained a `scale`, resolved at compile,
because `SUM` over a `DECIMAL` folds unscaled integers and the fold sits
outside the executor with no catalog to ask when it re-attaches the scale
in `Finish`. The merge path (AG-M) needed nothing: both partitions carry
the same item, so the same scale.

*Unrelated finding, recorded because it bounds what a test may assume:* a
fixture on `InMemoryPageStore` gets **two** relations. The third `CREATE
TABLE` fails with "page id already in use" at any bootstrap page count,
with tens of thousands of pages free, and reproduces with three plain
`int64` tables. It does **not** reproduce on `DevicePageStore` — the same
statements against the real server all succeed — so it is a limitation of
the test store, not of the engine.

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
