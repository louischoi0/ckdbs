# KDS Query Language v2 — Parser, Step Chains, and Execution

**Official specification** of the query language: the parser, the
AST→`StepChain` compile contract, and the execution contract. Consistent
with `docs/rules/rules.md`, `docs/spec/protocol.md` (KWP §5),
`docs/spec/waystone-concpets.md` (trail model and replay contract),
`docs/spec/heap-and-tuple.md` (invariants 7 and 11), `docs/spec/txn.md`,
`docs/spec/aggregate.md`, `docs/spec/join-inner-build.md` and
`docs/spec/null.md`.

The language runs: the step compiler (`src/exec/step_compiler.cpp`), the
step VM (`src/exec/step_vm.cpp`), joins, predicate-position subqueries, the
tri-state collapse, the row-touch budget (`include/kds/exec/budget.hpp`) and
pagination are in the tree with `tests/exec_chain_test.cpp`,
`tests/exec_subquery_test.cpp`, `tests/exec_budget_test.cpp` and
`tests/pagination_exec_test.cpp` beside them. The parser is recursive
descent over an owning AST; tokens are zero-copy views into the statement
and the fingerprint is accumulated during the parse. The corpus
(`tests/testdata/parser_corpus.txt`) is the enumeration of verdicts, verdict
by verdict.

Two facts a reader needs that no item below otherwise states: `IN (value
list)` is refused — `InvalidArgument`, "expected a subquery" — while
`IN (SELECT …)` and `BETWEEN` are served; and `CREATE TABLE` takes no
`WITH (...)` option table (I7).

---

## 0. Decision record

| # | Decision | Choice |
|---|---|---|
| J1 | Subquery scope | **Wide**: predicate-position subqueries — scalar (uncorrelated *and* correlated), `IN`/`NOT IN (SELECT …)`, `EXISTS`/`NOT EXISTS` — nesting to a fixed depth cap. Not restricted to flattenable forms |
| J2 | Inexecutable forms | Refused, understood, with an exact position — never a slow generic path. Table-position nesting (derived tables, CTEs), subqueries containing aggregates (**AG8**), and over-depth are truthful errors |
| J3 | Classification | Absorbed into **`kJoinSelect`**; the concept generalizes from "join chain" to **step chain**. No new enum value |
| J4 | Vehicle | The AST→`StepChain` compile contract is the seam: the parser emits an AST, `Compile(AST) → StepChain` resolves names against the catalog, and nothing downstream sees a name |
| J5 | Trail recording policy | **n = 2**: an instance's trail is recorded on its **second** execution (the first only counts) |

**J2's one code is two.** Everywhere this document writes a refusal, read the
*code* off the rule that splits it (`include/kds/base/status.hpp`,
`docs/spec/protocol.md` §11): `Unsupported` is what this engine's
architecture cannot admit and no later release lifts, `NotImplemented` is
what the design admits and nobody built. The position requirement, the
never-a-generic-path requirement and every form's membership in J2 hold for
both. **Most of J2 is `NotImplemented`** — table-position nesting (derived
tables and CTEs), outer joins, `ORDER BY` by ordinal or expression, the
aggregated-output tail, subqueries containing aggregates, and
`ORDER BY`/`LIMIT` inside a subquery. **What is `Unsupported`** is the
shorter list: the depth cap (a recursion needs *a* bound structurally, and
the parser, the compiler and the step VM agree on it), one relation named
twice in a FROM list without distinct aliases (distinct aliases are the
answer on every later release too), and a `$name` parameter on the newline
protocol, whose wire has no bind step.

## 1. The step chain

Every SELECT-class statement compiles to a **step chain**: an ordered list of steps, each reading one relation with one access kind. Written order is execution order — *the statement is the chain*.

| Kind | Authoritative work | Trail-replayable? |
|---|---|---|
| `Lookup` | pk-equality descent (constant or bound param) | **yes** — completeness follows from pk uniqueness |
| `Probe` | pk-equality descent keyed by a value produced by an earlier step or outer row | **yes** — same argument, per producing row, **and only under rule 0** (I17) |
| `Range` | pk range via the leaf chain | no — search; prefetch only |
| `Scan` | heap chain or full scan with a predicate | no — search; prefetch only |
| `Exists` | semi-join probe: stop at the first qualifying row | **positive result only** (§4) |
| `NotExists` | anti-join: prove absence | **never** — absence has no witness |

A join contributes one `Lookup`/`Probe`/`Scan` step per relation in written order. This table *is* `docs/spec/waystone-concpets.md` §2's trust model — a waystone may replace a lookup, never a search — extended with the negation rule: **negation steps are search-class by definition**.

Step numbering is global in compile order (the outer chain and every sub-chain share one counter), so a trail entry's `step_id` is unambiguous without parent linkage. The chain layout is a pure function of the AST, hence of `pattern_id`, which is what makes a recorded trail replayable across executions of one instance.

## 2. Subqueries as steps

- **Uncorrelated subquery** → a **prefix sub-chain**: hoisted, executed exactly once before the outer chain, its result bound as a value (scalar) or a probe set (`IN`).
- **Correlated subquery** → a **nested sub-chain step**: executed once per outer row, correlation columns arriving as `Probe` keys. A correlated pk-equality subquery therefore costs what a join step costs and records the same trail shape.
- **Scalar cardinality:** more than one row is a runtime error, `CardinalityViolation` (retryable = 0). Parse time cannot prove cardinality in general, so the check is per execution. Zero rows is NULL, and therefore a false predicate. A first-row pick is never acceptable — it makes the answer depend on physical order.
- **`IN` (positive)** compiles to `Exists` per outer row over the sub-chain, or a hoisted probe set when uncorrelated. **`NOT IN` / `NOT EXISTS`** compile to `NotExists`. `NOT IN` keeps standard three-valued semantics — any NULL in the subquery result makes the predicate never-true — implemented, tested, and called out in the client manual as the foot-gun it is.
- **Depth cap:** sub-chains nest to depth **4** (`parser::kMaxSubqueryDepth`); deeper is `Unsupported`.
- **Out of scope (J2), each refused with an exact position:** subqueries in FROM (derived tables), CTEs, subqueries containing `GROUP BY`/aggregates (**AG8** — a fold inside a sub-chain puts an aggregation boundary where the execution model has none), and subqueries in `INSERT`/`UPDATE` value position.

**Why table-position nesting stays out**, since it is the question every reader asks next: a derived table's result must become a relation with a schema, materialized somewhere and probed by something other than a pk. That breaks pk-direct probing into the next step, which is the entire shape of the execution model, and it puts a temporary relation in the storage layer. A predicate-position subquery needs none of it — it consumes rows and yields a boolean or a value.

The structural rule that enforces it: **the relation-reference production must never reach the statement production**, and `WITH` must not lex as a statement head (a statement beginning `WITH` is `NotImplemented`).

## 3. Architecture instructions

**I1 — Literals are arguments, never shape (`pattern_id` for free).**
The fingerprint is one pass over the token stream, taken during the parse (`include/kds/parser/fingerprint.hpp`): `pattern_id` hashes the *shape* — every token in order, with each value literal replaced by a single parameter marker — and `arg_hash` hashes the *arguments*, the inline literal values in the order they appeared. Identifiers are shape. Inline-literal and bind-parameter forms converge on one `pattern_id`: `WHERE id = 42` and `WHERE id = ?` are one pattern, and a `?` contributes nothing to `arg_hash` because its value arrives at BIND; `param_count` says how many bound values a BIND stage folds in before the instance key is final. A correlation reference contributes a *shape* marker to `pattern_id` and nothing to `arg_hash`, so a correlated statement keeps one stable `arg_hash` across outer rows — which is what gives it an instance key at all.

No `kFingerprintVersion` bump follows from any grammar addition: new keywords, `.`, and sub-select parens are additive shape, and every previously-accepted statement hashes identically. That invariance is pinned by the corpus, not assumed.

**I2 — One class per statement; the chain is the shape. (J3)**
The parser classifies execution shape at parse time and the executor dispatches on it with a `switch` — no plan enumeration exists. **Every step-chain statement classifies as `kJoinSelect`**, read as "step-chain select"; single-relation point and range forms keep their own classes. The enum is `StatementClass { kPointSelect, kRangeSelect, kJoinSelect, kUnclassified }` (`include/kds/exec/step_chain.hpp`) and does not grow: root classes for every outer shape × inner shape × correlation state would be an unbounded cross product, and `sys.patterns.stmt_class` is one byte on disk. The `StepChain` carries the shape; the tag carries the dispatch. `kUnclassified` is the safety valve and is metered.

**I3 — Per-session arena and flat AST.** Not built: AST nodes are owned by the tree the parser returns. Nothing here is a contract.

**I4 — Zero-copy tokenizing.**
Tokens reference the statement text in place (`include/kds/parser/token.hpp`); nothing is copied during lexing, and a token must not outlive its statement — the rule is structural (tokens never leave a parse), not enforced by a debug check. The one boundary where things outlive the parse (names entering the catalog, cached text) is the only place copies happen. The lexer tracks byte offsets so every error position — and every refusal position J2 requires — is exact. `.` is a token, so `a.x` lexes as a qualified name.

**I5 — Name resolution happens once, before execution.**
Relations resolve to oids and columns to ordinals; the executor never sees a name. A bound column reference is

```
ColumnRef { uint8 up; uint8 rel_slot; uint16 col_pos; }   // 4 bytes, no arena reference
```

`up` is a de Bruijn level — 0 is this chain, 1 its parent — so a predicate is independent of which chain it landed in, and it maps one-to-one onto the execute-time frame stack. Resolution rules: `a.x` names a relation or alias in this chain's FROM list or an enclosing one; an unqualified `x` resolves iff exactly one visible relation has that column, searching innermost-first and stopping at the first level that matches, so adding a column to an outer relation can never silently change an inner chain's meaning; anything else is `InvalidArgument` with the exact position. **Aliases (`FROM t AS a`) are in scope**, which is what makes a self-join expressible; a FROM list naming one relation twice without distinct aliases is `Unsupported`, not a silent ambiguity.

This lives in the **step compiler**, not the parser — `Compile(AST) → StepChain` resolves names against the catalog and emits `ColumnRef`s. A compiled `StepChain` contains no identifier on any execute path; execution performs zero catalog name lookups.

**I6 — Enforce the 40-bit pk range at the front door (invariant 7).**
Any literal or parameter destined for a pk position is range-checked `< 2^40`, judged on the raw digit text rather than a signed decode, rejected with `InvalidArgument` and the exact position. Nothing downstream re-checks. A probe key taken from a producing row needs no check — it came out of a Keystone word and is in range by construction. Out-of-range pk literals fail at parse, out-of-range pk binds fail at BIND with the exact parameter index.

**I18 — The error codes this language requires.**
`StatusCode::kUnsupported`, `kNotImplemented` and `kCardinalityViolation` exist (`include/kds/base/status.hpp`), each mapped to its own `wire::ErrorCategory` with `retryable = 0`; `kTxnConflict` remains the only retryable code. Every J2 form and every over-cardinality scalar returns its own code, never a generic `InvalidArgument`: a client cannot fix a cardinality violation by fixing its arguments.

## 4. Grammar surface

**I7 — `CREATE TABLE` options.** There is no `WITH (key = value, …)` option table; the storage form (`HEAP`/`BTREE`) is a trailing clause. **Waystone is not a table option**: it is keyed on `(pattern_id, arg_hash)`, not on a relation.

**I8 — Session and admin statements.** `SET DURABILITY {STRICT|GROUP|RELAXED}`, `SET ISOLATION LEVEL …`, `SHOW META`, `SHOW TABLES` and the other `SHOW` forms are *dispatcher* commands, not parser statements: ordinary statements returning ordinary result sets — one surface, one auth story. SET is excluded from fingerprinting.

**I9 — Join and nesting scope.**
§2's four subquery forms are in scope for predicate position, correlated included. Table-position nesting is out and answers `NotImplemented`.

The join surface: inner equi-join chains over 2+ relations (`JOIN … ON a.col = b.col`, chained and flat) with `AS` aliases; `ON` requires a qualified column (`rel.col`). `LEFT`/`RIGHT`/`FULL`/`OUTER` are lexed and reserved but rejected `NotImplemented` with the keyword's own position, so clients get a truthful error and the grammar does not shift.

**I10 — Filter scope.** Conjunctions of `col op {value}`, `BETWEEN`, `IS [NOT] NULL`, and the §2 subquery predicates. `OR`, `NOT` outside the reserved negation forms, and arbitrary expression trees are excluded — they blur classification, which is worth more than expressiveness here. `IN (value list)` is refused (`InvalidArgument`, "expected a subquery"); `IN (SELECT …)` is §2's.

**I11 — `ORDER BY <col> [ASC|DESC] [, ...] LIMIT n [OFFSET m]`.** Limit and offset are values, so `LIMIT 10` and `LIMIT 20` share a `pattern_id` — corpus-pinned. A sort *column*, being an identifier, is part of the shape instead: `ORDER BY a` and `ORDER BY b` are different patterns. **Each clause is independently optional**: `LIMIT` without `ORDER BY` is well-defined here in a way general SQL cannot promise, because I12 already makes emission order a client contract, so the clause takes a prefix of an order the statement has. Any column may be a sort key, pk or not, each with its own direction, up to **8 keys** (`parser::kMaxSortKeys`); pk order is the free order and the compiler **elides** the sort for `<driving pk> ASC` alone, every other order costing an output sort (`exec::OutputSort`, a sink decorator) bounded by `sort_max_rows` (default 1,048,576, `exec::kDefaultSortMaxRows`) — a sort that would exceed it fails the statement, and under `LIMIT` a top-N heap holds only what the reply needs. NULLs sort largest (`null.md` §4). `ORDER BY` by ordinal or expression is `NotImplemented`. **The tail is the outermost non-aggregated block's**: over aggregated output `ORDER BY` is parsed and refused at compile, `LIMIT`/`OFFSET` refused at parse, each `NotImplemented` (fold order is not a contract — `aggregate.md` §2), and inside a subquery the whole tail is `NotImplemented` with the byte. Which relation a sort column belongs to is the compile half's check, not the parser's. `LIMIT`'s execution is a sink-decorator quota over `RowSink`'s `kStop` — `exec::EmissionQuota`. The defining contract: the reply to `LIMIT n OFFSET m` is rows [m, m+n) of the unlimited reply. That contract binds the *reply*, not the chain, so it holds over the one row source that is not a chain: a `sys.*` statement is answered by the catalog's typed readers before the compiler is asked for a chain, and the quota runs over its materialized rows after the `WHERE` and before the formatting — the same `EmissionQuota`, constructed from the clause rather than from a `StepChain`. `ORDER BY` over a `sys.*` view is refused with the byte: `exec::OutputSort` normalizes its keys out of a `ChainFrame` against `SortKey`s resolved to indices in a Schema, and a view has neither. A clause the engine cannot serve is refused, never parsed and silently dropped.

**I13 — Dialect compatibility is a non-goal.** No PG/MySQL emulation, no quoting mimicry, no shims. Compatibility requests are product decisions, not parser patches.

## 5. Execution contract

**I12 — Written order is the plan ("the statement is the chain").**
Execution order is textual order: the chain runs front to back, nested-looping into each subsequent step. A *documented client contract* — deterministic, predictable, appliance-appropriate — not a limitation to apologize for. An uncorrelated sub-chain is hoisted and runs once before the outer chain; a correlated one runs per outer row, where it is written.

**Decorrelation rewrites are forbidden**, not merely unimplemented: turning a correlated subquery into a semi-join, or hoisting its inner side into a read the written order never scheduled, is exactly the silent reordering this contract rules out, and it is not validatable against the trail model. What it forbids is the *hoist* — reading the inner relation before written order reaches it. The statement-local inner build (sanctioned below) hashes inside the very walk written order schedules, which is why it is not this rewrite. The one permitted plan-level shortcut, because it changes no result and no order: a false uncorrelated top-level `EXISTS` short-circuits the statement without opening the outer relation.

**`ORDER BY` is the one sanctioned reordering, and it refines this contract rather than replacing it.** A sort is a sink decorator: the chain still runs front to back and still emits in written order, and the reordering happens strictly downstream of every step, every trail entry and every access count. Arrival order is the sort's last key and always ascending, so **rows the clause does not distinguish come back in the order this contract gives them** — a client that orders by a column with ties still sees written order within each tie. What a sort does *not* do is license the engine to reorder anything the statement did not ask to reorder; the prohibition above is unchanged. The contract is stated in the client manual, and execution order provably matches written order for both orderings of one query.

**Equality propagation is the one sanctioned predicate rewrite, and it adds conjuncts, never removes, rewrites or reorders one.** From `A = B` — two columns of one chain — and `B = <literal>`, the compiler appends the implied `A = <literal>` to the flat conjunct list before attachment, so the step owning `A` can be *keyed* on it: a join whose restriction is written against the other relation stops compiling the keyed side to a full walk per outer row. Its constraints, each load-bearing:

- **Results are unchanged by transitivity.** Joins are inner-only (I9) and no `NULL` exists to make `=` non-transitive; the derived conjunct is *appended*, so the residual still carries every written conjunct and any step downgraded to a scan filters to identical rows.
- **Plans may only be strengthened.** Derived conjuncts sit after every written one, and the pk-equality choice takes the first usable equality in residual order — so a written key is never displaced by a derived one. A derived conjunct *can* do more than fill a scan: it may promote a written `kRange` to a `kLookup` (a strictly stronger trust class under invariant 9), change which secondary index a step enters, reach a `CabinProbe`, or flip a statement's class to `kPointSelect` when it proves the driving step a point. Every one of those is a stronger access to the same rows; none weakens one.
- **At most one conjunct is derived per column**, and only for a column carrying no written equality-to-literal of its own — the first descriptor-matching literal in its class, in written order. A second literal on one column is plan-inert (a keyed candidate already exists) and result-inert (the written conjuncts fully express the predicate, contradiction included); without the bound, a class of *M* columns carrying *L* literals appends `L·(M−1)` conjuncts, which is a compile-time and per-row blowup an adversarial statement can drive to seconds.
- **Still `f(shape, catalog)`.** First-seen order everywhere, no data consulted; `docs/spec/index.md` §9's purity claim is unchanged.
- **The literal crosses only an identical type descriptor** (`type_val` and `len` both equal): it was coerced against the column it was written on and is copied bytes-for-bytes — re-coercing a coerced decimal would rescale it twice. A mixed-descriptor join key keeps its unpropagated plan.
- **`$param` propagates**, for the reason the lookup path treats one as pk-eligible.
- **Derived conjuncts are marked** (`StepPredicate::derived`): `ANALYZE` prints `derived` on their filter lines — a diagnostic must name a predicate the client can find in their text, and the derived occurrence's verdict is never different, since the descriptor guard makes both columns the same type.

Out of scope by decision: deriving column-column conjuncts (placement already keys on the earliest available side) and propagating range bounds through a join key. What this rewrite deliberately does not touch is order: the chain still runs front to back in written order, and the written order still decides which relation drives — propagation makes both writings of a restricted join fast, it does not choose between them.

**The statement-local inner build is the one sanctioned execution-time structure, and it adds a way to *locate* an inner match set, never a reorder of a row or a read** (`docs/spec/join-inner-build.md`). When a join's inner step would walk per outer row — no index, no Cabin, no literal to propagate — the executor lets the first outer row's inner walk double as the build of a statement-lifetime map from join-column values to matching rows, and every later outer row probes the map instead of walking. Three facts keep it inside I12, each load-bearing:

- **The outer relation still drives.** The build changes how a match set is located, never which relation iterates or what joins what — the claim IX17 and CB12 already ratified for correlated probes, applied to a structure whose lifetime is one statement.
- **Read scheduling is unchanged.** The inner relation is first read exactly when written order reads it: the build is that walk's side effect, the Recording pattern `docs/spec/cabin.md` §4 ratified.
- **Emission order is captured, not reconstructed.** Buckets append in walk order, so a probe replays each key's matches in the order the walk would have emitted them — for a named key and an issued one alike, since build order *is* the walk's order.

Hard rules ratified with it: never the outer side, never an emission reorder, never survives the statement, never feeds Waystone. What the prohibition above still forbids is the hoist — building before written order first reads the inner side — which is the semi-join/hash-join rewrite and is not what this paragraph licenses.

**I15 — Nested access rules.**
A nested sub-chain step and a join probe both perform storage access *inside* an outer walk's callback, and the buffer pool evicts (`docs/spec/eviction.md`), so a span into a page frame does not survive a nested fetch.

- **R1 — decode before descending.** At every step, the row is decoded into that step's owning frame buffer before any nested access. No span into a page frame — a tuple payload, or a descent's carried-out leaf — may be live across a call that can fetch another page.
- **R2 — nested steps are read-only.** Every nested walk uses read access, enforced structurally: the nested driver has no parameter for it. This makes the Halloween problem unreachable rather than merely absent, and stops a read-only statement from dirtying frames.
- **R3 — recursion is bounded**, at compile *and* at execute: the §2 depth cap is `parser::kMaxSubqueryDepth`, refused at each end. The chain-hop budget is a corruption guard, not a plan guard; the plan guard is §7's row-touch budget.
- **R4 — a walk must be stoppable.** `ChainVisit`/`BtreeVisit` callbacks return an outcome, and "stop" ends the walk successfully — `Exists` ("stop at the first qualifying row"), `LIMIT` and every cost guard depend on it. **"Stop" is never encoded as a `Status`** — no cancellation code exists engine-side, deliberately, and control flow through the error channel makes "did it fail or did it finish?" unanswerable at the call site.

A stopping visitor touches no page beyond the one it stopped on.

**I16 — NULL and cardinality.**
NULL is storable (`docs/spec/null.md`), and comparison is three-valued. The evaluator computes a tri-state and **collapses it to a boolean in exactly one place** — UNKNOWN becomes false at the conjunct, one function, one call site (`null.md` §4: `WHERE` keeps only true). `NOT IN`'s standard semantics are three-valued and the dangerous half is exactly that: if the subquery result contains a NULL and the probe matches nothing, `x NOT IN (S)` is UNKNOWN, not TRUE, so it is never implemented as `!IN`.

## 6. Trail integration

- **Recording (J5, n = 2):** the first execution of an instance `(pattern_id, arg_hash)` only counts; the second records. Sightings live in a bounded, core-local in-memory table; eviction merely restarts the count, which is a performance event. `sys.patterns.use_count` continues independently for retention.
- **Per-step recording:** `Lookup`/`Probe` steps append entries in execution order with their `step_id` and per-entry `rel_oid` — the existing 32-byte format, unchanged. `Exists` steps record the witnessing row only. `Range`/`Scan`/`NotExists` record nothing.
- **Replay** consults the instance's trail for entries with the step's `step_id` and applies `docs/spec/waystone-concpets.md` §2 per entry; any miss falls through to the authoritative path *for that step alone*. Search-class steps use the trail only as a prefetch batch.
- **`Exists` replay is positive-only**, and the asymmetry is the whole point: a validated witness *proves* non-emptiness, because presence has a witness. A missing or invalid witness proves nothing and the probe runs. A trail can never conclude absence.

**I17 — Rule 0: the probe key must be re-derived.**
Before a trail entry for a `Probe` step may be trusted, the executor derives the probe key from the **current** producing row it has in hand and requires it to equal the entry's `pk`. A mismatch is a miss for that step alone.

Without it, replay is a wrong-answer generator, and no other rule catches it. Suppose the producing row's join column was updated from 77 to 91 between recording and replay. The entry for pk 77 passes every other check in `waystone-concpets.md` §2 — `rel_oid` matches, the Keystone id at the recorded slot is 77, the epoch matches, MVCC says visible — because **every other rule validates the trail against storage and none of them looks at the query**. `UPDATE` overwrites in place and keeps `(page_id, slot)`, so nothing about the producing row looks stale. The join would emit row 77; the correct answer is row 91.

The check is free at runtime: the producing row is already decoded by R1 and the probe key is already a resolved `ColumnRef`. It is `waystone-concpets.md` §2's rule 0, built as the replay index's lookup key (`include/kds/exec/trail_replay.hpp`, keyed on `(step_id, pk)`), so an entry can only be found by matching the freshly derived key and there is no separate check to forget.

## 7. Executor

A small step VM in `src/exec/`: compile once (AST → `StepChain`, alongside class tagging), then iterate — a linear loop over steps, a nested loop for sub-chain steps, run to completion on the owning core. Cursor and row state live in the chain frame: no allocation per row, bounded by depth cap × per-step state. The VM is the compile contract J4 preserves.

Two things the VM inherits from I15 rather than choosing: frames own their decoded rows (R1), and nested steps are read-only (R2). One thing it must not do: hold a `TableAccess` pointer across anything that can bump the catalog version.

**Cost.** Nothing suspends mid-statement on a cooperative core, so an unbounded correlated scan is a denial of service on a shared engine. A per-statement row-touch budget (`max_rows_touched`, default `exec::kDefaultRowTouchBudget` = 100,000,000) stops the walk through R4 and fails the statement with a clear status; failing is the kinder answer.

## 8. Open decisions — do not assume

The decisions this section listed are unrecorded here.

## 9. Testing requirements

1. **Oracle equivalence:** every step-chain statement checked against a naive reference evaluator over small fixtures — joins, each subquery form, NULL-in-`NOT IN`, cardinality violations.
2. **Advisory family:** results byte-identical with Waystone off, trails dropped mid-run, and recording disabled — across nested chains too.
3. **Replay rules:** §1's per-kind table enforced by instrumentation — a `NotExists` step provably never reads a trail; an `Exists` step never concludes absence from one; rule 0's stale-probe-key case returns the new row.
4. **n = 2:** first execution records nothing (instrumented); second records; sighting-table eviction restarts the count with no correctness change.
5. **Refusal surfaces:** every J2 form errors with its own code and an exact position, and nothing falls through to a generic path.
6. **Depth and numbering:** chains at the cap; global `step_id` stability across executions of one instance.
7. **Fingerprint invariance:** every pre-existing corpus statement's `pattern_id` is unchanged by every grammar addition; convergence of inline and bound forms; a correlated statement's `arg_hash` stable across outer rows.
8. **Execution equivalence:** every case runs heap×heap, heap×btree and btree×btree with identical rows in identical order; `Probe` and `Scan` strategies agree row-for-row; execution order matches written order for both orderings of one query.
9. **Nested access:** a stopping visitor touches no page past the stop; a false uncorrelated `EXISTS` opens zero pages of the outer relation; a correlated `EXISTS` short-circuits, proven by page-touch count.
10. **Grammar:** golden parse trees for every production; byte- and token-level fuzzing — never crashes, always returns `Status`; a corpus of nesting attempts outside predicate position contains no accepted parse.
11. **Zero-copy:** token text is a view into the statement and lexing allocates nothing (`include/kds/parser/token.hpp`); the fingerprint is accumulated during the parse rather than by a second lex.

## 10. Required amendments (gate)

Nothing pending: every amendment this section once listed is in the document that owns it.

## Addendum — `DELETE`

`DELETE FROM <t> [WHERE <cond> [AND <cond>]*]` is a statement head.

- **The WHERE is the same production `UPDATE` uses** (`ParseOptionalWhere`
  at depth 0), compiled through the same `exec::CompileWhere`, so a
  `DELETE`'s predicate means exactly what the `SELECT` that found the rows
  meant. Predicate-position subqueries nest exactly as a `SELECT`'s do.
- **No fingerprint bump.** `fingerprint.hpp`'s rule names this exact case:
  making a statement fingerprintable that previously was not does not move
  any hash already stored. `DELETE` is not fingerprinted, and the
  golden corpus records it as `ok  -  -`.
- **`DELETE` is not a reserved word.** Like `SELECT`, `INSERT` and
  `UPDATE`, it is matched by text where the grammar expects it, so a column
  may still be named `delete`.
- It does not compile to a step chain. `HandleDelete` walks the relation
  itself, exactly as `HandleUpdate` does, and applies the same visibility
  predicate through `txn::Classify`.
