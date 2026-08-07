# Types — Workplan

Work instructions, companion to `docs/spec-types.md` (TY1–TY9). Tasks
`TY01`–`TY09`, **all built as of 2026-08-07**; `TY10` (TY3's phase 2)
and `TY11` (TY2's wide decimal), appended after closure, **built the same
day**.

(This line said `docs/types.md` until TY09, and TY09's own line said
`docs/aggregate.md`. Neither file has ever existed — see TY09's outcome.)

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

*A self-inflicted detour worth writing down, since the wrong diagnosis
was recorded twice before the right one.* This task's fixture could not
create a third table — "page id already in use", at any bootstrap page
count, with the whole store free. It was called an engine defect, then a
limitation of `InMemoryPageStore`, and it is **neither**:
`InMemoryPageStore`'s default `first_new_page_id` is 1, which sits inside
the catalog's fixed pages (0..13) and the reserved catalog-overflow range
(14..127), so allocation re-issues ids already in use. Every other
dispatcher-level fixture constructs it with `server::kFirstUserPageId`;
this one did not. The lesson is the one AP02 and AP03 already paid for in
the aggregate workplan — **re-measure the premise before believing it** —
and the tell was there in the first probe: the failure was insensitive to
page count, which exhaustion never is.

## TY06 — Rendering (needs TY04) — **DONE**

`FormatValue(type_val, value)` per §3.3, `0` preserving every existing
call site (mechanical sweep, no behavior change for existing types).

*Done when:* §6.7's pinned formats pass; the sweep is provably complete
(no remaining single-argument caller); scale-faithful trailing zeros
(`12.30`) and the timestamp fractional rule are pinned as `[PROPOSED]`.

*Outcome.* `SELECT` on a date column shows `2026-08-07` where it showed
`20672`. The stored form did not move and neither did decode - the
integer becomes a date at the emission boundary and nowhere earlier,
which is what keeps a scan from building text for rows it rejects.

**The single-argument overload is deleted, not defaulted**, which is how
"the sweep is provably complete" became a compiler check rather than a
grep. A defaulted parameter would let a caller that *should* pass a
column type go on rendering an epoch day silently; without one, every
call site had to be visited, and the ones with no column type say
`/*type_val=*/0` where that is the answer: a plan's literal (which is an
epoch integer by TY05, so rendering it as a date would show something the
chain does not contain), a catalog view's values, a group label in an
error message.

**`StepChain::projection_types`** carries the projected columns' types,
resolved at compile beside `column_names` and for the same reason - the
emission boundary must not ask the catalog once per column per row, which
is the per-row cost decode was kept free of. `SELECT *` carries none and
renders from the schema the dispatcher already resolved; the two paths
are pinned to agree. The fold's output renders through its own
`AggregateItem::type_val`, so `MIN(d)` is a date and the `COUNT(*)`
beside it is an integer.

**A `DECIMAL` ignores `type_val` entirely** - it is the one kind carrying
its own scale - which is what lets a column read and a `SUM`'s folded
output render identically with no caller knowing which it holds.

*Verified end to end on the real server*, not only through the test
store: `SELECT *`, a named projection, a `BETWEEN` over dates, and
`MIN/MAX/COUNT/SUM` all render as written, `SUM(d)` is refused with its
position, and a bad literal names its byte.

## TY07 — End to end through the dispatcher (needs TY03–TY06) — **DONE**

INSERT / SELECT / UPDATE / JOIN / GROUP BY / Cabin / Waystone over the new
types, wired and tested at the dispatcher level. Nothing here should need
new engine code — this task exists to prove §1's claim that the four
type primitives are all anything consumes.

*Done when:* an aggregated date-key statement groups first-seen; a Cabin
on a DATE column records, serves and drops per its contract test pattern;
a trail over a decimal-residual probe chain replays; SUM-over-DATE and
mixed-scale refusals surface through the wire with positions.

*Outcome: the task did the job it was written to do — it failed.*
`tests/types_e2e_test.cpp` caught a **row-losing bug in the Cabin write
hook**, which is the one failure mode `feat-cabin.md` §5 singles out as
invisible without a baseline to compare against: an observed value served
*fewer* rows than existed and looked entirely plausible doing it.

**The cause was TY05's, not the Cabin's.** TY05 made the step compiler
coerce a predicate's literal to its column's storage form, so a read of a
`DATE` column keys on the epoch integer 20514. The write hook was left
keying on `values[at]` — the literal **as written**, still the string
`'2026-03-02'`. The two keys never met. The value stayed observed, its
entry set stopped growing, and every row inserted after the observation
was dropped from the answer. Rows written *before* it were still served,
which is what made the result look sane.

The same root cause silently disabled §5's third row: the "did this
UPDATE touch the key column?" check compared a decoded integer against
the string it was written as, never compared equal, and so every write
appended — the unbounded growth that check exists to prevent.

**The fix is structural, not a second call site.** The coercion moved out
of `step_compiler.cpp` into `exec::CoerceLiteralToColumn`
(`row_codec.hpp`), beside the encoder whose parsers it shares, and both
paths now call it. The rule to keep: *any path that turns a written
literal into a value the engine compares or keys on goes through that one
function.* Two call sites that agree today are what this bug was.

*What the rest of the suite confirms* — the fold groups a `DATE` key
first-seen and renders it as a date, `SUM` over `DECIMAL` is exact at
scale through the unscaled accumulator, a join carries typed columns
through a chain frame at a non-zero slot, a trail over a decimal residual
replays byte-identically across three passes, and the refusals reach the
wire with their positions. None of that needed engine code, which is §1's
claim holding everywhere except the one place it did not.

## TY08 — Contract suite (needs TY01–TY07) — **DONE**

`tests/types_contract_test.cpp` collecting spec §6 items 1–8, regression-
mandatory, byte-for-byte comparisons in the established contract-test
style.

*Done when:* all eight pinned and in the default test target.

*Outcome.* `tests/types_contract_test.cpp`, one test per numbered item in
§6's order, in `kds_tests` and so regression-mandatory. It is deliberately
**not** a second copy of the unit tests: `types_predicate_test.cpp` checks
*how* a statement compiles and `types_e2e_test.cpp` checks that no
subsystem needed teaching, while this file checks the **product claims**
at the boundary a client sees. Where they overlap the overlap is the
point — a contract that holds only because some other file happens to
cover it is not pinned.

Three items needed more than transcription. **Item 2's ordering** is
pinned with data whose text order and value order disagree (`2026-11-05`
before `2026-02-09`, `9.90` before `100.00`), because a comparison that
had regressed to string collation would pass any test whose data happened
to sort the same way both times. **Item 4** wanted a *positioned*
refusal and the mixed-scale error did not carry one — `CoercePredicate`
now takes the byte offset, from the condition's column in a `WHERE` and
from the `ON` clause's left column in a join, which is where a reader
looks for the join they wrote. **Item 8's restart** re-mounts the same
store through a fresh `Catalog`, so `(p, s)` is read back from the pages
by something that never saw the `CREATE`; the test also writes and
predicates *after* the remount, which a cached-but-wrong `(p, s)` would
fail.

*Item 8's second clause is vacuous by TY02's decision and is recorded as
such rather than skipped:* "a pre-types data file opens and serves
unchanged, or refuses with the version message". TY02 packed `(p, s)`
into the existing `len` field precisely so there would be **no format
change and no version bump**, so there is no version message to pin and
no behaviour change to observe. What is pinned instead is the property
that made that choice safe — the packing survives a remount.

## TY09 — Docs & spec closure (needs TY08) — **DONE**

`docs/aggregate.md`: AVG's refusal message drops the "no decimal kind"
clause and §10 gains the AVG-return-type item (spec §3.2). `docs/types.md`:
ratify or amend the `[PROPOSED]` ranges/renderings from whatever TY07's
tests forced, with the change cited.

*Done when:* the two specs agree with the code and with each other; grep
for the old refusal text finds nothing.

*Outcome.* Six sites, and the task's own description was wrong about the
first two.

**The AVG refusal message never carried the stale clause.** It says only
"compute it from SUM and COUNT, which are exact", and always did. The
"no decimal kind" reason lived in two *code comments*
(`src/parser/parser.cpp`, `include/kds/parser/ast.hpp`), in
`feat-aggregate.md`'s AG2 row and its refusal table, and in `CLAUDE.md`.
All five now give the reason that actually holds — and the refusal itself
**stands**, which is the point worth being clear about: gaining a decimal
kind removed AVG's *stated* obstacle without removing its real one.

**`feat-aggregate.md` §10 gained the item**, written as a decision rather
than a placeholder: the return scale of `AVG` over `DECIMAL(p,s)`, the
rounding rule at that scale, and divide semantics over an integer column
are three questions with one answer, and none of them follows from having
a decimal type. Until they are settled, `SUM`/`COUNT` are exact and a
client computing the quotient picks its own rounding — worse ergonomics
and a better answer than picking one for them silently.

**`docs/aggregate.md` does not exist and never did.** This workplan's
TY09 line cited it, `spec-types.md` cited it twice (once in its
related-documents header), and `workplan-aggregate.md` had already
recorded the same slip from the other side. `spec-types.md` also cited
`docs/types-workplan.md` for its own workplan, which is
`docs/workplan-types.md`. Both are fixed, with a note saying so — a
broken cross-reference survives precisely because nobody follows it.

**The `[PROPOSED]` markers are ratified, by the contract suite rather
than by a client.** §6.1's ranges and §3.3's fractional-digit rule are
`[CONFIRMED 2026-08-07]`: TY08 pins both range edges, the two rejections
just outside them, and the rendering rule, so these numbers are
load-bearing now and moving one breaks a test on purpose. Widening the
`DATE` range stays cheap (a signed epoch day has room); narrowing it is
data-losing and needs a migration story.

*Two things fixed that were outside the brief but wrong:* `CLAUDE.md` had
**no types entry at all**, so a future agent reading it would not know
these types exist — it now has one under Core Architecture and one under
Documents, including the `CoerceLiteralToColumn` rule TY07's bug earned.
And `client-manual.md` still told clients the executor supports "no
`float`/`decimal` values, single-page heaps only"; both limits are gone,
and only `float` and NULLs remain.

## TY10 — Bare numeric literals (TY3 phase 2; needs TY01–TY09) — **DONE**

The one spec item TY09's closure left explicitly open and gated: a bare
`12.34` token, admissible only with its own fingerprint analysis. This is
the sanctioned exception to this workplan's lexer-off-limits rule — the
rule forbade touching the lexer *in service of TY01–TY09*, and phase 2 is
by definition the task that touches it.

*Done when:* `12.34` parses everywhere `'12.34'` does and means the same
thing; the fingerprint analysis is written where it is load-bearing; the
golden corpus passes unchanged for every pre-existing line;
`kFingerprintVersion` is unmoved or the bump is argued.

*Outcome.* Built as **one sentence with no case analysis: a bare numeric
literal is the quoted string of its spelling, exactly.** The lexer fuses
`digits . digits` into one `kNumLit` (both sides mandatory — `12.` and
`.5` lex as they always did, which is what keeps qualified-name grammar
and every previously-parsing statement token-identical); the parser's
`ParseValue` produces the kStr value the quoted form produces; the
fingerprint tags it `ShapeTag::kValue` and hashes `ArgTag::kStr` over the
same bytes. So `= 12.34` and `= '12.34'` share a pattern_id *and* an
arg_hash — one statement, not a collision — and every downstream stage
(coercion, encode, Cabin keys, trails) has exactly one case, which is how
TY07's two-keys bug class is kept structurally impossible here.

**The fingerprint analysis found the subtle case and it is the reason
this was gated.** `12.34` *lexed* before this token — int, dot, int, all
valid — so statements containing it were fingerprintable and their hashes
moved. No bump: those hashes were never *storable*, because int-dot-int
parses in no production, only executed statements are recorded, and a
CREATE PATTERN body must itself parse. That refinement — "already
fingerprintable" means "already storable" — is now written into
`fingerprint.hpp`'s bump rule, argued at the kNumLit case in
`fingerprint.cpp`, and pinned three ways: the golden corpus (every
pre-existing line unchanged, nine new lines including the bare/quoted
hash-equality pair), `fingerprint_test.cpp`'s two new tests, and the
corpus header's newly named second permitted transition.

**No carve-outs, verified where it surprises**: a bare `1.5` into a
varchar column stores the string `1.5` — the e2e suite first wrote that
case expecting an error and the engine was right, because `'1.5'` into a
varchar was always a plain string and a carve-out would be a second
meaning for one token. Refusals ride the existing gates: scale overflow
and date/integer mismatches surface with the same positions and texts as
the quoted form, pinned end to end.

What phase 2 is *not*: scientific notation and leading-dot forms (spec
§7). The v1 hint message the spec promised for the parse error was never
built, and acceptance retired the error it would have decorated — recorded
in §2 rather than silently, in this workplan's own tradition of noting
where a task's description and the code disagreed.

## TY11 — The wide decimal: int128 for p > 18 (needs TY01–TY09) — **DONE**

TY2's "future separate type", built as specified: a different schema
constant coexisting with the 8-byte type, never a widening of it. Spec
§2a carries the decision record; this entry carries what building it
found.

*Done when:* `decimal(19..38, s)` declares, stores, reads back, compares,
groups, folds and crosses the wire; the narrow type's contracts are
byte-for-byte untouched; cross-width comparison is a compile-time refusal.

*Outcome.* Six things worth knowing.

**The declared precision selects the width at the one DDL site.**
`decimal(p, s)` maps p ≤ 18 to type_val 7 and 19..38 to the new
`kTypeValDecimalWide = 13`; `decimal128(p, s)` names the wide type
directly and refuses p ≤ 18 toward the narrow spelling. One declaration,
one type — and DESCRIBE renders the type a column got.

**`kDecimalWide` is a `ValueType`, not a flag**, and that choice did the
finding: every exhaustive switch was surfaced by the compiler
(`MakeCabinKey`, the fold's key encoder), and every branch-style site was
swept by hand (coercion, comparison, rendering, both row codecs). A width
flag on `kDecimal` would have let each of those silently read the low 64
bits.

**One parser body serves both widths.** The digit walk was templated on
its accumulator (int64 / Int128) with the cap as data - TY01's one-parser
rule surviving the split - so scale overflow, precision overflow and
malformed input answer identically at either width, pinned.

**The int64 accumulator did not widen.** SUM/AVG over the wide type fold
through an `Int128 sum_wide` beside the int64 `sum`, because §3.3's
overflow point is a product contract; both merge unconditionally (the
unused one adds zeros), the AVG divide is one template body at either
width, and the DISTINCT merge decodes an entry's own tag to pick its
accumulator.

**Building it caught a live narrow-type bug.** An integer literal wider
than int64 wraps in the lexer (documented, `token.hpp`), and
`CoerceLiteralToColumn` read the wrapped `int_val`: against a narrow
decimal, `= 36893488147419103232` coerced as 0 and **matched 0.00** - a
silently wrong answer predating this task. The wide arm reads the
preserved digit text; date, timestamp and narrow-decimal coercion now
refuse a wrapped literal; uint64's digit-text path was already correct
and is deliberately untouched.

**Purely additive, again**: no format change, no version bump, one new
`sys.types` row (`decimal128`, oid 33). A pre-wide data file renders an
unknown type_val 13 only for columns it cannot contain. The wire carries
16 LE bytes under the type's own type_oid with `(p, s)` in `type_mod` -
exactly the shape the KWP DECIMAL decision reserved for it two commits
earlier.
