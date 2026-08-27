# Workplan — `HAVING`, and `ORDER BY` over aggregated output

Owner doc for the **post-fold consumers**. Retracts `docs/spec/aggregate.md`
AG7 ("HAVING — not in v1") and settles the second half of that spec's §10
open item — *"decide with HAVING, since both are post-fold consumers and
should share the wrapper seam AG1 established"* — by deciding both here.
Amends `docs/workplan-order-by.md`'s refusal table (the "Over aggregated
output" row) and `docs/spec/parser-v2.md` I11's aggregated-tail sentence.

Planned 2026-08-24 in the `feat-having-part` worktree at `72b85ad`. Nothing
below is built yet; every "does" is a task.

## 0. Decision record — ratified 2026-08-24, before any code

| # | Decision | Choice |
|---|---|---|
| HV1 | Placement | **A sink decorator at the AG1 seam**, downstream of `Aggregator::Finish` and upstream of the reply's formatting. The `Aggregator` learns nothing: it folds, and something else decides which folded rows leave. Same argument AG1 makes for the fold and OB4 makes for the sort — a second place that reasons about statement shape is a second answer to what the statement does |
| HV2 | What `HAVING` may name | **Aggregates only**, over any resolvable column, *whether or not the select list names it*. A HAVING-only aggregate compiles as a **hidden trailing item** the fold computes and the sink does not emit. A predicate on a grouping key is **refused with its byte**: a group key is constant within its group, so `HAVING b = 3` is a second spelling of `WHERE b = 3` — and the WHERE spelling also cuts rows *before* the fold, which the HAVING spelling cannot. Liftable later without breaking anything, which is what refusing first buys |
| HV3 | Predicate shape | The WHERE grammar's flat **AND-only conjunct list**, the same six comparison operators, and `IS [NOT] NULL`. The right-hand side is a **literal only** — no column, no subquery, no aggregate-against-aggregate. Three-valued exactly as WHERE is (`docs/spec/null.md` §4): an unknown comparison keeps no group, and `IS NULL` is how a group whose `SUM` folded no non-NULL value is asked for |
| HV4 | `ORDER BY` over aggregated output | **Lifted, at the same seam.** A key is a grouping column or an aggregate (hidden allowed), each with its own `ASC`/`DESC`, up to the existing `kMaxSortKeys`. A bare non-grouped column is refused with AG5's reason; an ordinal stays refused with OB1's |
| HV5 | `LIMIT` / `OFFSET` over aggregated output | **Still refused, with their present message.** Serving them would make fold order a client contract, and AG6 deliberately makes it a *deterministic* order rather than a promised one. §5 records what HV4 newly makes decidable, and does not decide it |
| HV6 | The literal's type | Coerced at **compile**, against the aggregate's answer type, through the one coercion the engine has (`CoerceLiteralToColumn`). `SUM`/`MIN`/`MAX`/`AVG` answer at the argument column's type and scale, so the argument column *is* the coercion target; `COUNT` answers int64 and takes an integer literal, with a string refused at compile and positioned |
| HV7 | Fingerprint | **Nothing is reserved and `kFingerprintVersion` does not move.** `HAVING` stays an ordinary identifier matched by text at clause position, and a HAVING literal is a slot exactly as a WHERE literal is, so `HAVING COUNT(*) > 1` and `> 2` share a `pattern_id`. Two corpus lines flip `Unsupported` → `ok` **keeping both hashes** — the V08/V09/OB1 precedent |
| HV8 | Waystone, Cabin, class, cross-core | **Untouched.** AG10 and AG14 hold unchanged: the compiled steps, kinds, residuals and class of a statement with a HAVING are its unaggregated twin's. `read_columns` is the one field that grows, for the reason OB3 already made it the one field a sorted chain does not share — a value must be decoded to be folded. Spec §9.1's identity is "steps, kinds, residuals, class", and the contract test already excludes the decode mask by name |

## 1. What it is

```sql
SELECT tier, COUNT(*)   FROM h GROUP BY tier HAVING COUNT(*) > 1;
SELECT tier             FROM h GROUP BY tier HAVING SUM(qty) >= 100;
SELECT tier, SUM(qty)   FROM h GROUP BY tier HAVING SUM(qty) IS NULL;
SELECT COUNT(*)         FROM h HAVING COUNT(*) > 5;          -- the global form
SELECT tier, COUNT(*)   FROM h GROUP BY tier ORDER BY COUNT(*) DESC, tier ASC;
```

The second line is the one that decides the design: the fold must compute
`SUM(qty)` for a statement that never emits it. That is the hidden item, and
it is the same mechanism `ORDER BY COUNT(*)` needs when the select list does
not carry the count — one mechanism, two clauses, which is why they are
built together rather than twice.

## 2. What stays refused, and why each is a decision rather than a gap

| Form | Answer | Why |
|---|---|---|
| `HAVING b = 3` (a grouping key) | `Unsupported` + the key's byte | HV2. A second spelling of `WHERE b = 3`, and the worse one — WHERE cuts rows before the fold |
| `HAVING x = 3` (an ungrouped bare column) | `InvalidArgument` + byte | AG5, one clause over: there is no "any row" mode, and a value that depends on scan order is one this engine refuses to give |
| `HAVING COUNT(*) > COUNT(DISTINCT b)` | `Unsupported` + byte | HV3. An aggregate on both sides is an expression grammar's shape, and AG9's argument does not stop at GROUP BY |
| `HAVING COUNT(*) > (SELECT ...)` | `Unsupported` + byte | AG8's boundary. A sub-chain under a post-fold predicate puts an aggregation boundary where the execution model has none |
| `HAVING` inside a subquery | `Unsupported` + byte | Unreachable in practice — AG8 already refuses the fold — but stated so the refusal is the fold's and not a parse accident |
| `HAVING` over a `sys.*` view | refused by AG12's existing message | A view's rows never came from a chain, so there is no fold for a post-fold filter to sit behind |
| `LIMIT` / `OFFSET` over aggregated output | `Unsupported` + byte, message unchanged | HV5 |
| `ORDER BY 1` over aggregated output | `Unsupported` + byte | OB1's rule, unchanged: an ordinal is a second spelling of a name |
| `ORDER BY x` (ungrouped, non-aggregate) | `InvalidArgument` + byte | AG5 again — the fold has no such value to order by |

## 3. Tasks

### HV-1 — the grammar — **built 2026-08-24**

`src/parser/parser.cpp`, `include/kds/parser/ast.hpp`.

Landed as described below, with two things worth recording because they
were decided while building rather than while planning:

- **The interim refusal is the compiler's.** A clause that parses and is
  then silently dropped is the failure I11 records having already made once
  on a catalog view, so `CompileSelect` refuses a non-empty `having` and an
  aggregated `order_by` with the offending byte until HV-2 and HV-4 land.
  The refusal sits *outside* the `aggregated()` arm: a HAVING makes a
  statement aggregated at parse, so the two conditions coincide today, and
  a refusal reachable only through a second fact stops holding the day that
  fact changes. Client-visible behaviour is therefore unchanged by HV-1 —
  the same statements are refused, positioned, one layer down.
- **Two corpus verdicts flipped and neither hash moved**, which is HV7's
  evidence rather than a side effect: `parser_corpus.txt` lines 257-258 read
  `ok` now, with `pattern_id` and `arg_hash` byte-identical.

`HAVING` is read where AG7's refusal is read today: past the GROUP BY list,
before the pagination tail, matched by text and still unreserved. The
conjunct list reuses what exists — `ParseSelectItem()` already parses
exactly "an aggregate call or a plain column reference, with a byte offset",
which is the left-hand side HV2 and HV4 both need, and `ParseCompareOp()`
the middle.

Two AST additions, and the second is a reuse rather than a field:

- `SelectStmt::having`, a `std::vector<HavingCondition>` — a `SelectItem`
  left side, a `CompareOp`, an `AstValue` right side. Empty for every
  statement that wrote no clause, so nothing downstream changes shape.
- `SortKey` carries a `SelectItem` where it carries a `ColumnName` today.
  A `SelectItem` with `is_aggregate == false` *is* a column name with an
  offset, so the non-aggregated path keeps its meaning and its bytes; the
  aggregated path gets its aggregate keys with no second carrier and no
  `optional`. The migration is mechanical (`.column` → `.key.column`).

The parser judges **shape only**, as OB1 did: whether a name is a grouping
key, whether an aggregate typechecks, and what the literal must become are
catalog questions, and the compiler is where catalog knowledge lives. The
one thing the parser still decides is `aggregated`, which it already
computes — and **a `HAVING` makes a statement aggregated exactly as a GROUP
BY does**. That needs no refusal of its own and adds no case: `SELECT x FROM
t HAVING COUNT(*) > 5` becomes an aggregated statement with a bare ungrouped
column in its list, which AG5 already refuses with `x`'s byte, and
`SELECT COUNT(*) FROM t HAVING COUNT(*) > 5` was aggregated by its item
before the clause was read. One rule, no new message.

### HV-2 — the compiler

`src/exec/step_compiler.cpp` (`CompileAggregate`, `ReadColumnsOf`),
`include/kds/exec/step_chain.hpp`.

`AggregateSpec` grows three things:

```
std::vector<AggregateItem> items;     // visible items, then hidden ones
std::size_t visible_items = 0;        // the emitted prefix
std::vector<HavingPredicate> having;  // item index, op, coerced literal
```

`HavingPredicate` names an item by **index into `items`**, never by a
column reference: the predicate compares the fold's answer, and the fold's
answer is an item. Resolution is therefore three steps — resolve the
written aggregate as if it were a select item (the existing path, including
`CheckAggregateArgType` and its AVG/SUM refusals), match it against the
visible items by *resolved* identity (func, distinct, star, `ColumnRef`) so
`HAVING COUNT(*) > 1` beside a selected `COUNT(*)` costs no second state,
and append it as a hidden item when nothing matches.

`sort_keys` resolves the same way over an aggregated statement, and a key
that is a grouping column resolves to the *item* carrying that column when
one exists and to a hidden item otherwise — so the sort reads one row shape
and not two.

The literal is coerced here (HV6) and only here, with the position added
where the offset is. `ReadColumnsOf` notes a hidden item's column exactly as
it notes a visible one, which is where `read_columns` grows and HV8 says why
that is the field allowed to.

### HV-3 — the filter

`include/kds/exec/having.hpp`, `src/exec/having.cpp`.

One class, one question: `Accept(std::span<const parser::AstValue>) → bool`,
evaluating each `HavingPredicate` through `CompareValues(item.type_val, ...)`
— the comparison the whole engine already uses, three-valued collapse and
`IS [NOT] NULL` arms included. AND-only means the first false answer ends it.

No state, no allocation, and no knowledge of grouping: it is handed a
finished output row and answers whether it leaves.

### HV-4 — the sort, over the fold's rows

`include/kds/exec/sort.hpp`, `src/exec/sort.cpp`.

`OutputSort` gains a second arming — keys as **indices into the fold's
output row** rather than `ColumnRef`s — and a second `Admit` taking a
`std::span<const parser::AstValue>`. Everything below that is the existing
machinery: `OrderKeyOf`, `Before`, the arrival-order tiebreak, the
`sort_max_rows` refusal, `Finish`. **One comparator, as OB2 requires** —
a second ordering for aggregated output would be a second answer to which
value sorts first.

The top-N heap is kept but never binding here, because HV5 leaves the fold
without a `LIMIT`; `aggregate_max_groups` (65,536) bounds the row count long
before `sort_max_rows` (1,048,576) can, which is worth stating so nobody
later reads the cap as dead code.

### HV-5 — the dispatcher

`src/server/command_dispatcher.cpp` (`RunAggregated`, `RunAnalyze`).

`Finish`'s sink becomes the pipeline the decision record describes: filter,
then sort-or-render, then — for a sorted fold — drain in order after the
walk. Rendering emits `spec.visible_items` values, so a hidden item is
computed, compared, ordered by, and never seen.

`RunAnalyze` runs all of it, for AG15's reason unchanged: the run ANALYZE
describes is the run that happened, and a filter that drops half the groups
is not free.

### HV-6 — ANALYZE and the plan printer

`src/exec/plan_printer.cpp`.

The `aggregate` line gains `having=<n>` (predicates) and the `sort` line
prints an aggregated statement's keys by item rather than by column ref.
The report gains `kept=` beside the existing `groups=`: groups founded and
groups emitted are different numbers the moment a HAVING exists, and one
number cannot say both.

### HV-7 — tests

`tests/aggregate_contract_test.cpp`, `tests/parser_aggregate_test.cpp`,
`tests/testdata/parser_corpus.txt`, `tests/analyze_test.cpp`.

1. **Every row of §2's refusal table**, each with its byte — the table gets
   one owner, the way spec §2's already has one.
2. **Chain identity survives**: the chain compiled for a statement with a
   HAVING is byte-identical to its unaggregated twin's in steps, kinds,
   residuals and class, over the existing corpus of shapes. The hidden item
   moves `read_columns` and nothing else.
3. **The hidden item is not emitted**: `SELECT tier FROM h GROUP BY tier
   HAVING SUM(qty) >= 100` returns one column, and its rows are exactly the
   groups a client-side filter over `SELECT tier, SUM(qty)` would keep.
4. **NULL**: a group that folded no non-NULL argument is dropped by every
   relational operator and found by `IS NULL`.
5. **The global form**: `HAVING` over a no-GROUP-BY statement emits one row
   or none, and none over an empty relation is the standard's answer.
6. **Corpus**: line 257's `HAVING` case and line 258's aggregated `ORDER BY`
   case flip `Unsupported` → `ok` with `pattern_id` and `arg_hash` unmoved;
   two literals differing (`> 1` vs `> 2`) share a `pattern_id`.
7. **Waystone**: the five-configuration contract comparison, run over a
   HAVING statement and a sorted-fold statement — the replayed and the
   descended executions emit identical bytes, which is AG10 holding.
8. **Determinism**: two executions emit identical bytes, and an AG-M merge
   of two partitions filtered afterwards equals the one-pass fold filtered
   afterwards — the invariant HV1's placement is what preserves.
9. **ANALYZE**: `groups=` and `kept=` differ where the clause drops groups,
   and a HAVING that drops none leaves the plan otherwise unchanged.

### HV-8 — the measurement

`ck-tester`, `build-release`, interleaved A/B, per the measurement rule.
The question is overhead, and there are exactly two places it can hide:

- **A statement with no HAVING and no aggregated ORDER BY must not move.**
  The empty predicate list is one branch per group, not per row, so the
  expectation is "unmeasurable" and the job is to show it.
- **A HAVING statement against its client-side equivalent**: the same fold
  with the filter applied by the client. The clause should win by what it
  does not format and does not send, and the number belongs in the report
  whichever way it lands.

### HV-9 — the documents

`docs/spec/aggregate.md` (AG7 retracted in place with the date, §2's table,
§8's "what v1 is not", §10's open item closed), `docs/workplan-order-by.md`
(the refusal table's aggregated row), `docs/spec/parser-v2.md` I11 and the J2
paragraph naming HAVING, `manual/sql/sql.md` (both passages),
`docs/spec/client-manual.md`, and this repo's `CLAUDE.md` milestone row. A
retraction says what was claimed, what is true now, and the date — never a
silent edit.

## 4. Order of work, and where the reviews go

HV-1 → HV-2 → HV-3 → HV-4 → HV-5 → HV-6, then HV-7 and HV-8 over the whole.
A `critics-developer` review per completed step, not once at the end: HV-2's
hidden-item identity rule is the contract every later step is built on, and
finding it wrong after HV-5 costs all of them.

## 5. Open — do not assume

- **`LIMIT` over an aggregated statement that wrote an `ORDER BY`.** HV5
  refuses it, and HV4 is what makes refusing it a choice rather than a
  consequence: once the groups are ordered, rows [m, m+n) of that reply are
  well-defined without fold order being a contract. Deciding it means
  deciding whether `LIMIT` over an *unordered* fold stays refused beside it,
  which is a different question from the one this doc answered.
- **A grouping key in `HAVING`** (HV2's refusal) stays refusable-only-once:
  lifting it later is additive, and no client can have depended on the
  refusal.
- **Aggregates in `ORDER BY` for a non-aggregated statement** remain refused
  by OB1's paren rule, unchanged and unexamined here.
