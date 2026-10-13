# Work order BU — the basic query surface: `OR`, `NOT`, value lists, select-list expressions, `HAVING`, outer joins, set operations

Written 2026-10-13 on `worktree-bu-basic-query-surface` from `eeb88f47`
(`v2.7.0-992-geeb88f47`). It follows the operator's words of 2026-10-13,
recorded in English:

- **W0:** *"Now the SQL query surface has to be covered whole. The
  unsupported forms were a v1 limit; we are at v3 now, so the work starts."*
- **W1:** *"First, fill the most universal and essential query surface."*
- **W2:** *"Full agreement is not needed for now; first work on the basic
  query forms, excluding CTEs."*
- **W3:** *"Move the working directory to ckdbs, add the work order, and
  implement it the way kdbs development is already done."*

**Written and opened 2026-10-13 on W3.** W0-W2 stand here as BU-R1..R3.
The questions in §4 were unmarked when the order was written; W3 is the
word to build, and CLA's proposals are adopted under it. **Each is recorded
in §4's mark column as adopted on W3, never as the operator's own per-item
mark.** A finding that would change an adopted proposal goes back to the
operator before it is built.

BU was checked against every file name under `instructions/`, `docs/` and
`CLAUDE.md`, and every commit subject on every branch, for a `BU-` token or
a `workorder-bu` name at `eeb88f47`: no use found. The last letter assigned
is BT (`worktree-bt-accept-tuple-row`, not yet on `main`).

**Claim tags:** `[source-read]` with `path:line` at `eeb88f47`; `[design]`
for CLA's reasoning about code not yet written; `[measured]` with the
invocation; `[quiet-wrong]` marks an item where a wrong choice turns a
refusal into a wrong answer.

## 0. What BU is

### 0.1 Subject

The manual states the query grammar as a short list of absences: "no `OR`,
no `NOT` over expressions, no parenthesized nesting" (`manual/sql/sql.md`
§4 WHERE), no `IN` value list ("the open half of workplan V08"), no
select-list expressions or aliases, no `HAVING`, no outer joins, no derived
tables, no set operations. W0 rules those a v1 limit. BU builds the basic
forms of each, in the order of W1, and leaves CTEs out (W2).

The external evidence is anti-kdbs's census of 2026-10-13
(`/home/cdkbs/anti-kdbs/plans/sql-query-surface.md` §2, `tests/feature/F004-basic-query-forms.py`):
75 basic-form cases over a small dataset at `ec0c07ef`, of which 5 answer
and 70 are refused. BU closes when F004's 70 answer as PostgreSQL does, or
each remaining one is refused by a ruling in §4.

### 0.2 Not in BU

- CTEs (`WITH`), recursive or not (W2).
- Window functions, `LATERAL`, grouping sets.
- A **keyed** disjunction: BU-S1 lowers `OR` to a residual filter only
  (BU-Q1). `WHERE id = 1 OR id = 2` is a scan in BU. A later order may key
  it; BU's tests pin the answers that order must keep.
- `<>`, a bare alias `FROM t a`, quote escaping, `NULLS FIRST/LAST`:
  refused by decision (`docs/spec/parser-v2.md` I13 and the manual §7), not
  by lack of work (BU-Q5).

## 1. Survey at `eeb88f47`

### 1.1 A WHERE is a flat AND-list

`Parser::ParseOptionalWhere` (`src/parser/parser.cpp:1338`) parses
`ParseOneCondition` and loops on `AND`; anything else ends the clause, so
`OR` is reported as "unexpected token after end of statement" by the
statement head. `[source-read]` The AST is
`std::vector<parser::Condition>` (`include/kds/parser/ast.hpp:265`), one
`PredicateKind` per conjunct (`ast.hpp:210`): `kCompareValue`, the four
subquery kinds, `kBetween`, `kCompareFunction`.

### 1.2 Who reads a raw `Condition`

`[source-read]` Two kinds of consumer:

- **Lowering** - `exec::CompileWhere` (`src/exec/step_compiler.cpp:1483`,
  UPDATE and DELETE) and `CompileBlock`'s WHERE loop (`step_compiler.cpp:1712`,
  SELECT and every subquery). Each turns a `kCompareValue`/`kBetween` into
  `StepPredicate`s in a flat residual, a subquery into a `SubChain`, and a
  `kCompareFunction` into a `FunctionPredicate` in `Step::fn_residual`.
- **Raw readers** in `command_dispatcher.cpp`: `PkLiteral`/`PkEqualityTarget`
  (`:6591`, `:6620`, the point-write fast path), `DeclaredWriteBorrow` (the
  write's lock window), `FoldPkWindow` and `PurgeConjunctRefusal` (`:9570`,
  `:10090`, PURGE), and the catalog-view evaluator (`:7099`). Each tests
  `kind` first, as `ast.hpp:270`'s warning requires: twice a consumer that
  did not read `kBetween` as `col = low` and wrote the wrong row.

### 1.3 The function conjunct is the precedent

AP-S4 added a predicate that must never become a key: `DATE(ts) >= '...'`
read as `ts >= '...'` returns wrong rows (`include/kds/exec/step_chain.hpp:366`).
It was made **a residual kind of its own**, `Step::fn_residual`, which no
key, bound, index, Cabin, join-key or build reader walks, evaluated after
the residual at the two places every accepted row passes - `AcceptTupleAt`
(`src/exec/step_vm.cpp:2429`) and `EvaluateConjuncts` (`step_vm.cpp:2881`,
the UPDATE/DELETE path). Placement: the latest step any of its columns
reaches (`step_compiler.cpp:1839`). `[source-read]`

### 1.4 Three-valued logic is collapsed per comparison

`CompareValues` (`src/exec/row_codec.cpp:1288`) answers false for a
relational operator with a NULL operand - the collapse `null.md` §4 asks
for, at the conjunct. Under `NOT` that collapse is wrong: `NOT (a = 1)`
must not keep a row whose `a` is NULL. The sub-chain evaluator already
carries a `TriState` (`step_vm.cpp:575`). `[source-read]`

### 1.5 The fingerprint is a token stream

`src/parser/fingerprint.cpp` hashes the shape token by token, so `OR`, `NOT`
and parentheses enter a statement's shape without a new tag, and literals
stay values. A statement that did not parse had no `pattern_id`, so
admitting it moves no stored hash (`fingerprint.hpp`'s bump rule). No
`kFingerprintVersion` bump. `[source-read]`

## 2. Rulings

### BU-R1 — The whole query surface, basic forms first (W0, W1)

The forms of §3 are built in that order. Each stage's forms answer what the
SQL standard answers (PostgreSQL as the reference dialect, BJ-R1's
precedent), under KDS's own contract where it speaks (no implicit
conversion, NULLs sort largest, `!=` only, written order is the plan).

### BU-R2 — Breadth first; full agreement later (W2)

A stage is done when its forms answer the plain cases right and refuse the
rest with a position - not when every interaction is proved. The deep
differential work is anti-kdbs's (F003, R003) and later orders'.

### BU-R3 — No CTE (W2)

`WITH` keeps its refusal by name.

### BU-R4 — Three-valued logic, collapsed once, at the root `[quiet-wrong]`

A boolean tree evaluates in Kleene logic - `NOT unknown` is unknown,
`false AND unknown` is false, `true OR unknown` is true - and UNKNOWN
becomes false only where the tree meets the conjunct list. `parser-v2.md`
I16 and `null.md` §4 are amended to say "at the root of each conjunct".

### BU-R5 — A disjunction is never a key `[quiet-wrong]`

A conjunct that holds `OR` or `NOT` is lowered to a residual kind of its
own (BU-Q1 (a)), the function conjunct's precedent (§1.3): no pk bound,
index range, Cabin probe, join key, build key, write lock window, PURGE
window or point-write target is read off it, and equality propagation
(`parser-v2.md` I12) draws only from top-level conjuncts. A top-level AND
of plain comparisons keeps every access path it has today.

### BU-R6 — Unbuilt is refused, positioned

Every form a stage does not build answers `NotImplemented` naming its byte,
never a parse error and never a half-applied predicate. `PURGE` keeps
refusing every conjunct that is not a pk window, `OR` included.

## 3. Stages

| Stage | Builds | Gate |
|---|---|---|
| **BU-S0** | this order | written |
| **BU-S1** | `OR`, `NOT <predicate>`, parentheses over the non-subquery predicates (comparison, column pair, `IS [NOT] NULL`, `BETWEEN`, function comparison), in `SELECT`, every subquery `WHERE`, `UPDATE`, `DELETE`. A subquery predicate under `OR`/`NOT`, and `OR` over a catalog view, refused `NotImplemented` | F004 1a cases (but `EXISTS` under `OR`) answer; full suite |
| **BU-S2** | `IN (v, ...)`, `NOT IN (v, ...)`, `NOT BETWEEN`, `LIKE`/`NOT LIKE`, a literal on the left of a comparison | F004 1b, and 1c's `LIKE` and literal-left cases |
| **BU-S3** | the subquery predicates under `OR`/`NOT` | F004 W06 |
| **BU-S4** | select list: column alias, literal, the BJ value expressions, `DISTINCT`, `rel.*`; `ORDER BY` an expression, alias or ordinal; expression operands in WHERE | F004 phase 2, W24, W25 |
| **BU-S5** | `HAVING`, `ORDER BY`/`LIMIT` over groups, aggregate alias, aggregate over an expression, aggregate in a subquery | F004 phase 3 |
| **BU-S6** | `INNER JOIN`, `CROSS JOIN` and comma joins, `ON` with several conditions or a non-equality, `USING`; `LEFT`/`RIGHT`/`FULL` outer joins | F004 phase 4 |
| **BU-S7** | `UNION [ALL]`, `INTERSECT`, `EXCEPT`, derived tables, a scalar subquery in the select list | F004 phase 5 |
| **BU-S8** | `INSERT` with a column list, `INSERT ... SELECT`, expressions in `VALUES`, `RETURNING`; `UPDATE ... FROM`, `DELETE ... USING`, `ON CONFLICT DO NOTHING` | F004 phase 6 |
| **BU-S9** | the close: specs, manual, census, overhead measured once | - |

Each stage runs the Session Workflow: a `critics-developer` review of the
stage's commit, the full suite, a row in §6. The overhead is measured once,
at BU-S9 (`raft-marks-2026-09-30.md` §8); each stage lands with "overhead
not measured; measured at the milestone's close".

## 4. Items for the operator

| # | Question | CLA's proposal | Mark |
|---|---|---|---|
| BU-Q0 | Open BU whole, or stage by stage | (a) whole, stages in §3's order | adopted on W3 |
| BU-Q1 | Access path of a conjunct holding `OR`/`NOT`: (a) residual only, (b) a union of keyed probes | (a): the function conjunct's shape, provably never a key; (b) is a later order's, against BU's pinned answers | adopted on W3 |
| BU-Q2 | Size of a boolean tree | the BJ expression caps (`src/parser/parser.cpp:310`): 64 levels of nesting, 4,096 nodes, refused `Unsupported` at the byte | adopted on W3 |
| BU-Q3 | `NOT` over a single comparison: keep it a tree node, or rewrite it to the negated operator | keep the node: `NOT (a = 1)` and `a != 1` agree in Kleene logic, but a rewrite is a second spelling the plan printer must explain | adopted on W3 |
| BU-Q4 | `OR` over a catalog view (`sys.*`) | refused `NotImplemented` in S1: the view evaluator compares `col op val` by name (`command_dispatcher.cpp:7099`) | adopted on W3 |
| BU-Q5 | `<>`, bare alias, quote escaping, `NULLS FIRST/LAST` | stay refused (I13) | adopted on W3 |
| BU-Q6 | `LIKE` | bytes, case-sensitive, `%` and `_`, no `ESCAPE` clause (refused `NotImplemented`) | adopted on W3 |
| BU-Q7 | Outer-join placement of a `WHERE` conjunct on the nullable side | never moved into the `ON`; equality propagation never crosses the nullable side `[quiet-wrong]` | adopted on W3 |

## 5. Measurement

Once, at BU-S9, over BU's whole code change: `ck-tester`'s interleaved A/B
in `build-release`, the AND-only statements of the existing benchmarks
against the commit BU opened from. BU-R5 predicts no change for them; the
measurement is what says so.

## 6. Row status

### BU-S0 — written 2026-10-13

This document, on `worktree-bu-basic-query-surface` from `eeb88f47`.
