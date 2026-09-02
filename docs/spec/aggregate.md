# KDS Aggregation — GROUP BY & Aggregate Functions (Specification)

Official specification, decisions AG1–AG15 (§0). Consistent with
`docs/spec/parser-v2.md` (J-series, step chains), `docs/rules/rules.md`,
`docs/spec/waystone-concpets.md` (trail model), `docs/spec/cabin.md`,
`docs/spec/crosscore.md` (CC-series), `docs/spec/txn.md`, `docs/spec/types.md`.

Where this document writes a refusal, the code is one of a pair
(`include/kds/base/status.hpp`, `docs/spec/protocol.md` §11): `Unsupported`
for what the architecture cannot admit, `NotImplemented` for what the design
admits and nobody built. Each refusal below names its code.

## 0. Decision Record

| # | Decision | Choice |
|---|---|---|
| AG1 | Placement | **A fold outside the executor.** The dispatcher wraps the statement's `RowSink` in an `Aggregator`; the compiled chain is byte-identical to the same statement without the fold. The executor never learns aggregation exists. **Invariant: every aggregate state is mergeable** (§1) |
| AG2 | Functions | `COUNT(*)`, `COUNT(col)`, `SUM(col)`, `MIN(col)`, `MAX(col)`, `AVG(col)`, **with `DISTINCT`** (`COUNT(DISTINCT col)`, `SUM(DISTINCT col)`, `AVG(DISTINCT col)`; `MIN`/`MAX` accept it as the standard's no-op). `AVG` answers at the argument column's declared scale, rounded half-even, so decimal columns only — an integer column is refused at compile (§3.4). `AVG(DISTINCT col)` divides the distinct sum by the distinct count over one set |
| AG3 | SUM arithmetic | **Checked int64** (int128 for `DECIMAL128`, `types.md` §2a). Signed integer and decimal argument columns; the fold uses overflow-checked addition and an overflow is a **statement error**, never a wrapped number. `SUM` over a `uint64` column is `Unsupported` (half its range does not fit the accumulator); over `DATE`/`TIMESTAMP` it is `InvalidArgument`. Documented product constraints (§3.3) |
| AG4 | NULL semantics | **SQL standard** (§3.1): aggregates skip NULLs; `COUNT(*)` counts rows; a group with no non-NULL argument yields NULL for `SUM`/`MIN`/`MAX`/`AVG`; NULL grouping keys form one group |
| AG5 | Strict grouping | A bare column in an aggregated select list **must appear in GROUP BY**, or the statement is refused with the column's byte position. There is no "any row" mode: an answer that depends on scan order is an answer this engine refuses to give |
| AG6 | Emit order | **First-seen order** — the order the chain's deterministic row stream founded each group. Hash-iteration order would vary by seed and growth history, which the deterministic-test rule forbids |
| AG7 | HAVING | Parsed for shape — `HAVING <agg or column> <op> <literal> [AND …]`, `IS [NOT] NULL` admitted, an expression, subquery, column or second aggregate on the right side refused at parse — and the statement is then **refused at compile with `NotImplemented`** and the clause's position. A `HAVING` makes a statement aggregated exactly as a `GROUP BY` does |
| AG8 | Subquery aggregates | **J2 stands**: a subquery containing GROUP BY or an aggregate answers `NotImplemented` with the position of whichever half made the block aggregated — a fold inside a sub-chain puts an aggregation boundary where the execution model has none |
| AG9 | Grouping targets | **Column references only** (`col`, `rel.col`). The grammar has no expressions, and GROUP BY does not grow one; a `(` after a GROUP BY name is `NotImplemented` |
| AG10 | Waystone / Cabin / patterns | **Unchanged, and free.** AG1 makes the chain identical, so recording, replay, Cabin probes, access statistics and pattern registration hold for an aggregated statement exactly as for the same statement without the fold. `IsTrailReplayable` and `HasReplayableStep` do not move |
| AG11 | Memory bound | **A cap that refuses, never truncates or spills** — the same discipline Cabin's caps and J2's no-slow-path rule already state. Config keys `aggregate_max_groups` (default 65,536) and `aggregate_max_distinct` (total DISTINCT entries per statement, default 1,048,576); exceeding either fails the statement with a truthful error naming the key |
| AG12 | Catalog views | Aggregation over `sys.*` is refused: a view's rows come from the catalog's readers, not a chain, so there is nothing for AG1's fold to wrap |
| AG13 | DISTINCT | Merged into AG2, semantics in §3.2 |
| AG14 | Statement class | **Unchanged.** Aggregation is consumption shape, and the rule "projection shape must never affect the class" extends to it: an aggregated statement classifies exactly as its chain does. No new `StatementClass` value, so `sys.patterns.stmt_class` keeps its meaning |
| AG15 | ANALYZE | The plan printer has one `Aggregate` line (keys, items, DISTINCT flags). ANALYZE runs the identical parse/compile/execute; the fold's group count joins the report |

---

## 1. Placement — the fold and the merge invariant

Aggregation is a **consumer of the row stream**, not a step kind. The
dispatcher, which already owns the statement's `RowSink`, wraps it: the sink
body becomes `Aggregator::Accumulate(frame)`, and after the chain completes,
`Aggregator::Finish(emit)` produces the output rows. This is the same seam
Waystone lives outside of, and for the same reason — a second place that
reasons about statement shape is a second answer to "what does this statement
do".

What this buys, stated so it can be tested: the chain compiled for
`SELECT b, COUNT(*) FROM t GROUP BY b` and the chain compiled for
`SELECT b FROM t` are identical in steps, kinds, residuals and class. Every
property proved of the chain — trail replay, Cabin probes, the
scan/probe equivalence, "downgrading any step to a scan cannot change the
result" — therefore holds for aggregated statements **without a new proof**.

**Invariant AG-M (mergeable state).** Every aggregate's running state must
support `Merge(a, b) → a'` such that folding a row stream in one pass and
folding two disjoint partitions of it then merging yield the same output
rows. `COUNT`/`SUM` merge by addition, `MIN`/`MAX` by comparison, `DISTINCT`
by set union, `AVG` as its `(sum, count)` pair with the divide deferred to
`Finish`. It is what lets `docs/spec/crosscore.md`'s step pipeline ship
*partial aggregates* — group count, not row count, on the wire — without
touching the step VM. Merge preserves the left operand's group order and
appends the right's unseen groups in their own order, so first-seen
determinism (AG6) survives a merge with a defined partition order.

What AG1 deliberately gives up: **pre-aggregation below a join is rejected.**
That optimization needs an operator inside the chain, and buying it means
teaching the step-kind trust table about a step that reads no relation.

## 2. Grammar

```
select      ::= SELECT select_list FROM rel join* [WHERE cond (AND cond)*]
                [GROUP BY column (, column)*]
select_list ::= * | select_item (, select_item)*
select_item ::= column | agg
agg         ::= COUNT ( [DISTINCT] column | * )
              | SUM   ( [DISTINCT] column )
              | MIN   ( [DISTINCT] column )
              | MAX   ( [DISTINCT] column )
              | AVG   ( [DISTINCT] column )      -- decimal columns only (§3.4)
column      ::= name | rel_binding . name
```

**Every new word is unreserved**, matched by text exactly as `SELECT` and
`FROM` are (`token.hpp`'s deliberate non-reservation list). A column may
still be named `count` or `group`: a function head is recognized only as an
unqualified name from the function set **followed by `(`** — no production
puts a paren after a column reference — and `GROUP` is read as a clause head
only after the WHERE, where no column reference can stand.

**Fingerprint invariance.** Because nothing is reserved and no token type is
added, every previously-accepted statement lexes to the same token stream
and hashes identically. **No `kFingerprintVersion` bump** — every stored
`pattern_id` and every recorded waystone survives. Pinned by a golden-corpus
test, not assumed.

Refusals, each with an exact byte position:

| Form | Answer |
|---|---|
| `SELECT * … GROUP BY a` | `InvalidArgument` — which columns `*` folds was never written |
| bare column not in GROUP BY | `InvalidArgument` (AG5) |
| duplicate GROUP BY column | `InvalidArgument` — always a slip, and it doubles the key encoding for nothing |
| `GROUP BY` on an expression | `NotImplemented` (AG9) |
| `AVG(…)` over a non-decimal column | `InvalidArgument` at compile (§3.4) — the column declared no scale, and the refusal names the two honest options: declare `DECIMAL(p, s)`, or compute SUM and COUNT and choose your own rounding |
| `SUM(*)`, `MIN(*)`, `MAX(*)` | `InvalidArgument` — `*` is only an argument of COUNT |
| `COUNT(DISTINCT *)` | `InvalidArgument` — distinctness of whole rows was never written |
| `HAVING …` | `NotImplemented` at compile (AG7); a malformed right side `NotImplemented` or `InvalidArgument` at parse |
| aggregate inside a subquery | `NotImplemented` (AG8 / J2) |
| aggregate over `sys.*` | refused (AG12) |
| `ORDER BY` on an aggregated statement | parsed — an aggregate is admitted as a key — and `NotImplemented` at compile: an aggregated statement's output rows are not chain rows |
| `LIMIT` / `OFFSET` on an aggregated statement | `NotImplemented` at parse: groups are emitted in fold order, which is not a contract, so which rows survive the clause would be a guess |

## 3. Semantics

### 3.1 NULLs (AG4 — SQL standard)

| Function | NULL handling |
|---|---|
| `COUNT(*)` | counts rows; never NULL |
| `COUNT(col)` | counts rows whose `col` is not NULL; never NULL |
| `SUM/MIN/MAX/AVG(col)` | fold over the non-NULL values; **NULL** when the group has none |
| grouping key | NULL keys form **one group**, emitted like any other |

Global aggregate (no GROUP BY): exactly **one output row, even over empty
input** — `COUNT` 0, `SUM`/`MIN`/`MAX`/`AVG` NULL. With GROUP BY, empty
input emits **zero rows**. Both are the standard's answers and both are
pinned by contract tests.

Note the deliberate asymmetry with `CompareValues`' "NULL never matches":
predicates *compare* and grouping *encodes identity*. Two NULL keys are the
same group not because NULL equals NULL but because the key encoding is the
same bytes. No change to `CompareValues`.

### 3.2 DISTINCT (AG2/AG13)

`COUNT(DISTINCT col)` counts, and `SUM(DISTINCT col)` sums, each distinct
non-NULL value once per group. Distinctness is per `(group, item)`: the fold
keeps an observed-value set keyed by the same value encoding the group key
uses, so "distinct" means exactly what "same group key" means. `MIN`/`MAX`
accept `DISTINCT` and ignore it — the standard's reading, since an extreme
of a set equals the extreme of its support. Distinct sets count against
`aggregate_max_distinct` (AG11).

### 3.3 SUM arithmetic (AG3) — documented product constraints

The accumulator is **int64 with checked addition** (int128 beside it for
`DECIMAL128`). An overflow fails the statement: `ERR SUM overflow in group …`
with the aggregate's label. A wrapped sum is the one output this feature
must never produce — it is wrong in a way no reader can detect, which is
the same argument the trail trust model rests on.

Constraints, stated as product facts the way `keystoneid-invariant.md`
states the 40-bit budget: **(a)** `SUM` requires a signed integer column
(`int8`–`int64`) or a decimal column, whose unscaled value rides the same
checked adder (`types.md` TY2); **(b)** `SUM` over `uint64` — the pk type —
is `Unsupported`, because half its range exceeds the accumulator and a sum
of ids is a statement nobody meant; **(c)** `SUM` over `DATE`/`TIMESTAMP`
is `InvalidArgument` — not a value. `MIN`/`MAX` over `uint64` **are**
exact: the item carries its catalog `type_val`, and comparison goes through
the digit-text path `row_codec.cpp` provides for values above `INT64_MAX`.

### 3.4 AVG

One principle: **AVG never invents digits and never drops declared ones.**

- **Return scale.** `AVG(DECIMAL(p, s))` returns a decimal of scale `s` —
  the answer in exactly the units the schema declared. No guard digits: a
  wider answer would manufacture precision the column never claimed, and a
  fixed widening constant would be a number the engine defends forever. The
  "no guard digits" line is load-bearing: a client that has seen `avg(amt)`
  answer at scale `s` will parse it at scale `s` forever.
- **Rounding.** **Half to even** at that scale, computed exactly on the
  integer pair — the quotient of the unscaled sum by the count, ties to
  the even neighbor. Sign-symmetric and bias-free under accumulation; no
  float touches the value at any point. Pinned at the ties in both signs,
  because half-up would agree everywhere else.
- **Divide semantics / integer columns.** An integer column declared no
  scale, so any fractional answer invents digits and a whole-number one
  silently drops the remainder — **refused at compile**, naming the two
  honest options (declare `DECIMAL(p, s)`, or compute `SUM` and `COUNT`
  and choose your own rounding). `DECIMAL(p, 0)` **does** average — that
  scale was declared — and rounds to whole units under the same rule.
  `uint64`, dates, timestamps and text refuse as they do for SUM.

The state is the `(sum, count)` pair of §1: the sum rides the same
checked adder as SUM (overflow is the same statement error), the divide
runs **once, in `Finish`** — never per row, and never at merge, where
averaging two partial *quotients* would be unrecoverable rounding. NULLs
are skipped (AG4) and a group with no non-NULL argument answers NULL — an
average of nothing is an absence, and the divide-by-zero cannot arise
because the divide only runs when a value was seen. `AVG(DISTINCT col)`
is `SUM(DISTINCT)/COUNT(DISTINCT)` over **one** set, so both halves agree
about which values they averaged.

## 4. Compiled form

`StepChain` carries `std::optional<AggregateSpec>`:

```
AggregateSpec
  items[]        output row, written order:
                   { is_aggregate, func, star_arg, distinct,
                     ref: ColumnRef, type_val }
  group_keys[]   GROUP BY, written order (ColumnRef)
```

Resolution happens at compile, once, against the catalog — no identifier
survives onto the fold path (spec I11 extends unchanged). `column_names`
labels the fold's output (`b`, `count(*)`, `sum(distinct x)`), which is the
one place names survive, for the same reason they already did.
`StepChain::star()` is "projection empty **and** no aggregate", so the
dispatcher's `SELECT *` path cannot misread an aggregated statement.

Validation at compile (all positioned): AG5's grouping check, duplicate
group keys, AG3's type checks. The compile stays pure — same statement plus
same catalog, same spec, bit for bit — so the chain layout remains
`f(shape, catalog)` and `pattern_id` still names it.

## 5. The fold

Per input row: encode the group key into a **reused scratch buffer** (tag
byte per key — null/int/str — then the value bytes), one heterogeneous
hash-map probe, fold each item's state in place. **Zero allocations per
row**; allocation happens only when a row founds a new group. The
no-GROUP-BY form skips the map entirely: one state row, folded in place, no
key, no hash.

Groups live contiguously in a first-seen-ordered vector; the map holds
indices into it. `Finish` walks the vector — emit order is AG6's by
construction, not by sorting.

DISTINCT state is a per-`(group, item)` set over the same value encoding.
It exists only for items that declared DISTINCT; a statement without the
word pays nothing for the feature.

## 6. Bounds (AG11)

| Key | Meaning | Default |
|---|---|---|
| `aggregate_max_groups` | groups per statement | 65,536 |
| `aggregate_max_distinct` | DISTINCT entries per statement, summed over all sets | 1,048,576 |

`aggregate_max_groups` was chosen by measurement against the `bench/` tree
at `1769487`: the fold's cost is proportional to **group count**, not row
count, so a statement approaching the cap is already slow enough to be
visible — the cap is a backstop rather than a tuning knob — and the ceiling
works out at about 27 MB per statement. `aggregate_max_distinct`'s default
is not measured; the same arithmetic puts it at about 84 MB per statement.

Exceeding either **fails the statement** with the key's name in the error.
No spill, no truncation, no silent partial answer — a truncated group set
is a wrong answer with a right answer's shape, exactly the failure Cabin's
caps refuse. Both keys are server config (`kds.conf`), read once at boot
like `durability`.

## 7. System interactions

- **Waystone**: an aggregated point/probe chain records and replays its
  keyed steps unchanged; the fold consumes replayed rows and descended rows
  identically, because a hit and a miss already run identical code from the
  tuple onward. `HasReplayableStep` unmoved.
- **Cabin**: a `kCabinProbe` step under a fold serves exactly as under a
  stream; the residual re-check the fold's rows passed through is the same
  one §4 of `cabin.md` requires.
- **Patterns**: an aggregated statement fingerprints and registers as any
  other does.
- **Class / access stats**: AG14 — unchanged; `RecordChainAccess` sees the
  same chain.
- **Cross-core**: AG-M is the contract. A remote core runs a local
  `Aggregator` over its partition and ships states; the home core merges.
  The wire format for a shipped state is `crosscore.md`'s, not this
  document's.
- **Transactions**: the fold reads through the statement's snapshot like
  every read; nothing here touches visibility.

## 8. What v1 is not

`HAVING` (AG7) · `ORDER BY`/`LIMIT`/`OFFSET` over aggregated output (§2
table) · `AVG` over anything but a decimal column (§3.4) · expressions
anywhere (AG9) · aggregates in subqueries (AG8) · aggregation over catalog
views (AG12) · spill or partial answers (AG11) · pre-aggregation below
joins (§1) · persistence of anything — the fold is per statement and
leaves nothing behind.

## 9. Contract tests — done when

1. **Chain identity**: the chain compiled with and without the fold is
   byte-identical (steps, kinds, residuals, class) for a corpus spanning
   lookup, probe, range, cabin-probe, filter-scan and join shapes.
2. **Fingerprint invariance**: every statement in the golden corpus hashes
   identically before and after the grammar change; `kFingerprintVersion`
   unmoved.
3. **NULL table**: every row of §3.1 pinned, including the one-row-on-empty
   global form and the zero-rows-on-empty grouped form.
4. **DISTINCT**: `COUNT(DISTINCT)` / `SUM(DISTINCT)` against duplicated
   input; `MIN(DISTINCT)` equals `MIN`.
5. **Overflow**: a `SUM` crossing `INT64_MAX` fails with the documented
   error and no row is emitted; `MIN`/`MAX` over `uint64` above `INT64_MAX`
   are exact.
6. **Strict grouping**: each refusal row of §2's table, with its position.
7. **Determinism**: two executions over the same data emit identical bytes;
   a merge of two partitions (AG-M) equals the one-pass fold.
8. **Waystone**: an aggregated probe chain replays; the replayed and
   descended executions emit identical aggregate output (the five-way
   contract-test pattern `waystone_contract_test.cpp` already uses).
9. **Bounds**: exceeding each AG11 cap fails with the key's name and emits
   nothing.

## 10. Open items — do not assume

The decisions this section listed are unrecorded here.
