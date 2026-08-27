# KDS Query Language v2 — Parser, Step Chains, and Execution

**Status: official specification.** Supersedes **both** `docs/parser.md` (the v1 parser blueprint, I1-I14) and `docs/step-chains.md` (the step-chain spec, J1-J5, confirmed 2026-08-01), which are kept as history and must not be built from. Task breakdown: `docs/inflight/in-progress/parser-v2-workplan.md` (`V00`-`V32`), which supersedes `docs/parser-workplan.md` (`PR01`-`PR24`) and `step-chains-workplan.md` (`SC01`-`SC10`).

Written as *instructions*: each item states what to build and when it counts as done. `[CONFIRMED]` is settled; `[PROPOSED]` marks a default to amend before building if needed; `[OPEN]` must not be assumed. Consistent with `docs/rules/rules.md`, `docs/spec/protocol.md` (KWP §5), `docs/spec/waystone-concpets.md` (trail model and replay contract), `docs/spec/heap-and-tuple.md` (invariants 7 and 11), and `docs/spec/txn.md`.

**Why one document.** Two specs were written for one subject: a parser blueprint that declared nested structures out of scope, and a step-chain spec that reversed exactly that. Both were partly right, and reconciling them per-reader is how a codebase ends up with two half-built languages. The step-chain model wins on everything it covered — it is the more recent decision, it has a ratified decision record, and its execution model is better. The parser blueprint's architecture items survive intact, and four engine-level gaps that neither document named are now items here.

**Status of the work: built, less the items named below** (corrected 2026-08-10 — this line read "unbuilt" long after it stopped being true, which `docs/inflight/verified/status-audit-2026-08-08.md` called the most misleading line in `docs/`; per-task state is in `docs/inflight/in-progress/parser-v2-workplan.md`, which was right throughout).

The language runs. The step compiler (`src/exec/step_compiler.cpp`), the step VM (`src/exec/step_vm.cpp`), joins, predicate-position subqueries, the tri-state collapse, the row-touch budget (`include/kds/exec/budget.hpp`) and pagination are in the tree with `tests/exec_chain_test.cpp`, `tests/exec_subquery_test.cpp`, `tests/exec_budget_test.cpp` and `tests/pagination_exec_test.cpp` beside them. Two of phase V-6's items landed early out of Waystone's cost work (2026-08-03): the fingerprint rides the parse rather than lexing a second time, and tokens are zero-copy views.

What is **not** built, each named so it is not read as shipped:

- **V08's `IN (value list)`** — the open half; it still reports "expected a subquery".
- **V11** — `CREATE TABLE ... WITH (PHYSICAL_OPTIMIZER = ON|OFF)`. No option table exists in the parser, which is why `docs/spec/physical-optimizer.md` R12 says "once both exist".
- **V12** — `SET DURABILITY` and the `kSessionSet`/`kAdminShow` classes. `SHOW META`/`SHOW TABLES` exist as *dispatcher* commands, not as parser statements, and no spelling selects a durability class per transaction.
- **V20** — the written-order contract's test (`tests/exec_order_test.cpp` does not exist); the manual half is written.
- **Phase V-6's remainder** — the arena, the flat AST, the slot table, binding at PARSE, named statements (V24, V26-V28, V30-V32).

The historical baseline this document was written against — recursive descent over an owning AST and a copying lexer, `SELECT *` with AND-only `col op literal` filters, no join execution anywhere — is what phases V-2 through V-5 replaced.

---

## 0. Decision record `[CONFIRMED 2026-08-01]`

Carried forward from `docs/step-chains.md` §0 unchanged, with the merge note on J4.

| # | Decision | Choice |
|---|---|---|
| J1 | Subquery scope | **Wide**: predicate-position subqueries — scalar (uncorrelated *and* correlated), `IN`/`NOT IN (SELECT …)`, `EXISTS`/`NOT EXISTS` — nesting to a fixed depth cap. Not restricted to flattenable forms |
| J2 | Inexecutable forms | **`Unsupported`** with exact position — never a slow generic path. Table-position nesting (derived tables, CTEs), subqueries containing aggregates (**AG8**, permanent for v1), and over-depth are truthful errors |
| J3 | Classification | Absorbed into **`kJoinSelect`**; the concept generalizes from "join chain" to **step chain**. No new enum value |
| J4 | Vehicle | **Bolt-on to the current recursive-descent parser** — do not wait for the blueprint parser. The AST→`StepChain` compile contract is the seam that survives the parser replacement. *(Reaffirmed at the merge, 2026-08-01: the blueprint parser's arena/flat-AST work moves behind the language, not in front of it — phase V-6 of the workplan)* |
| J5 | Trail recording policy | **n = 2**: an instance's trail is recorded on its **second** execution (the first only counts) |

Four items are new here because both source documents depended on them and neither named them: `StatusCode::kUnsupported` does not exist (I18); a relation walk cannot be stopped (I15); nested access is re-entrant into the page layer and safe only by accident (I15); and the trail replay contract is **not sufficient for a `Probe` step** (I17, rule 0).

## 1. The step chain `[CONFIRMED]`

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

## 2. Subqueries as steps `[CONFIRMED shape]`

- **Uncorrelated subquery** → a **prefix sub-chain**: hoisted, executed exactly once before the outer chain, its result bound as a value (scalar) or a probe set (`IN`).
- **Correlated subquery** → a **nested sub-chain step**: executed once per outer row, correlation columns arriving as `Probe` keys. A correlated pk-equality subquery therefore costs what a join step costs and records the same trail shape.
- **Scalar cardinality:** more than one row is a runtime error, `CardinalityViolation` (retryable = 0). Parse time cannot prove cardinality in general, so the check is per execution. Zero rows is NULL, and therefore a false predicate. A first-row pick is never acceptable — it makes the answer depend on physical order.
- **`IN` (positive)** compiles to `Exists` per outer row over the sub-chain, or a hoisted probe set when uncorrelated. **`NOT IN` / `NOT EXISTS`** compile to `NotExists`. `NOT IN` keeps standard three-valued semantics — any NULL in the subquery result makes the predicate never-true — implemented, tested, and called out in the client manual as the foot-gun it is.
- **Depth cap:** sub-chains nest to depth **4** `[PROPOSED default]`; deeper is `Unsupported`.
- **Out of scope (J2), each `Unsupported` with an exact position:** subqueries in FROM (derived tables), CTEs, subqueries containing `GROUP BY`/aggregates (**AG8** — a fold inside a sub-chain puts an aggregation boundary where the execution model has none; permanent for v1, not blocked), subqueries in `INSERT`/`UPDATE` value position `[OPEN: revisit]`.

**Why table-position nesting stays out**, since it is the question every reader asks next: a derived table's result must become a relation with a schema, materialized somewhere and probed by something other than a pk. That breaks pk-direct probing into the next step, which is the entire shape of the execution model, and it puts a temporary relation in the storage layer. A predicate-position subquery needs none of it — it consumes rows and yields a boolean or a value.

The structural rule that enforces it: **the relation-reference production must never reach the statement production**, and `WITH` must not lex as a statement head.

## 3. Architecture instructions

**I1 — Parameterize literals at parse time (`pattern_id` for free).** `[eventual state; see the vehicle note]`
The lexer never places literal values into the AST. Every literal is pushed into a statement-local slot table; the node holds a slot index. The AST is therefore born normalized: `pattern_id` is the hash of the token/shape stream computed during the parse itself — no separate normalization pass. Inline-literal and bind-parameter forms converge on one `pattern_id`; `arg_hash` is computed over the unified value stream at BIND.

Slots have three kinds. `kLiteral` (value at parse) and `kBindParam` (value at `C_BIND`) both feed `arg_hash`. `kOuterRef` — a correlation reference — **is never written to**: it holds a column reference, contributes a distinct *shape* marker to `pattern_id`, and contributes **nothing** to `arg_hash`. Two things depend on that: the AST stays `const` through execution, so one cached statement can serve two portals; and a correlated statement keeps one stable `arg_hash` instead of a new one per outer row, which is what gives it an instance key at all.

*Vehicle note (J4):* under the bolt-on the fingerprint remains the separate pass it is today, and the slot table lands with the blueprint parser (V-6). **No `kFingerprintVersion` bump is required by any of this work**: new keywords, `.`, and sub-select parens are additive shape, and every previously-accepted statement hashes identically. That invariance is pinned by a test, not assumed.
*Done when:* `WHERE id = 42` and `WHERE id = ?` yield one `pattern_id`; a correlated statement's `arg_hash` is identical across executions touching different outer rows; the pre-existing corpus's `pattern_id`s are unchanged by every grammar addition.

**I2 — One class per statement; the chain is the shape. `[CONFIRMED — J3]`**
The parser classifies execution shape at parse time and the executor dispatches on it with a `switch` — no plan enumeration exists. **Every step-chain statement classifies as `kJoinSelect`**, read as "step-chain select"; single-relation point and range forms keep their existing classes. The enum does not grow.

This is what makes nesting describable without a class explosion: root classes for every outer shape × inner shape × correlation state would be an unbounded cross product, and `sys.patterns.stmt_class` is one byte on disk. The `StepChain` carries the shape; the tag carries the dispatch.

v2 class list `[PROPOSED]`, unchanged: `kPointSelect`, `kRangeSelect`, `kJoinSelect`, `kPointUpdate`, `kPointDelete`, `kInsert`, `kCreateTable`, `kSessionSet`, `kAdminShow`, `kUnclassified`.
*Done when:* every production maps to exactly one class; the executor contains no shape re-analysis (one `switch`, over stamped tags, found by grep); `kUnclassified` exists as the safety valve and is metered.

**I3 — Per-session arena + index-based flat AST (zero alloc).** `[phase V-6]`
AST nodes live in flat per-kind arrays owned by a per-session arena, reset per statement with capacity retained; references are indices, not pointers. A cached named statement is the arena snapshot — a bounded byte copy with no pointer fixups.

One structural finding, recorded now because it constrains the migration: contiguous `{first, count}` child ranges hold for every list **except conjuncts**. Parsing an outer conjunct descends into a sub-chain whose conjuncts land in the shared array first, interrupting the outer's run. Exactly one list can be interrupted, because a subquery is admissible only in predicate position (§2) — so conjuncts carry a **sibling link** and everything else stays contiguous.
*Done when:* an allocation-counting test shows zero allocations parsing a warm session's statement; a snapshot is a byte copy with zero fixups; interleaved conjunct emission yields correct sibling chains.

**I4 — Zero-copy tokenizing via `string_view` over KWP frames.** `[phase V-6 for the views; the byte offsets are needed earlier]`
Tokens reference the `C_PARSE` payload in place; nothing is copied during lexing. Views are valid for the statement's parse only, and the one boundary where things outlive it (names entering the catalog, cached text) is the only place copies happen. The lexer tracks byte offsets so every error position — and every `Unsupported` position J2 requires — is exact.

Note `.` lexes as an error today, so `a.x` cannot be tokenized at all; a dot token is a prerequisite for qualified names, and adding it is additive shape.
*Done when:* lexing performs no copies (instrumented); error positions point at the offending byte.

**I5 — Name resolution happens once, before execution.**
Relations resolve to oids and columns to ordinals; the executor never sees a name. A bound column reference is

```
ColumnRef { uint8 up; uint8 rel_slot; uint16 col_pos; }   // 4 bytes, no arena reference
```

`up` is a de Bruijn level — 0 is this chain, 1 its parent — so a predicate is independent of which chain it landed in, and it maps one-to-one onto the execute-time frame stack. Resolution rules: `a.x` names a relation or alias in this chain's FROM list or an enclosing one; an unqualified `x` resolves iff exactly one visible relation has that column, searching innermost-first and stopping at the first level that matches, so adding a column to an outer relation can never silently change an inner chain's meaning; anything else is `InvalidArgument` with the exact position. **Aliases (`FROM t AS a`) are in scope**, which is what makes a self-join expressible; a FROM list naming one relation twice without distinct aliases is `Unsupported`, not a silent ambiguity.

*Vehicle note (J4):* under the bolt-on this lands in the **step compiler**, not in the parser — `Compile(AST) → StepChain` resolves names against the catalog and emits `ColumnRef`s. When the blueprint parser arrives it binds at PARSE and feeds the same compiler, and nothing downstream changes. This is the compile contract J4 exists to protect.
*Done when:* a compiled `StepChain` contains no identifier on any execute path; execution performs zero catalog name lookups, counted.

**I6 — Enforce the 40-bit pk range at the front door (invariant 7).**
Any literal or parameter destined for a pk position is range-checked `< 2^40`, judged on the raw digit text rather than a signed decode, rejected with `InvalidArgument` and the exact position. Nothing downstream re-checks. A probe key taken from a producing row needs no check — it came out of a Keystone word and is in range by construction.
*Done when:* out-of-range pk literals fail at parse, out-of-range pk binds fail at BIND with the exact parameter index.

**I18 — The error codes this language requires. `[NEW]`**
Two codes do not exist and everything above depends on them:

- **`StatusCode::kUnsupported`** — J2's entire surface promises it, and the enum has eight codes plus `kTxnConflict`. `include/kds/wire/kwp.hpp` currently states outright that `kUnsupported` has no engine-level equivalent; that note is corrected with the addition. Appending is free — nothing persists a `StatusCode`.
- **`CardinalityViolation`** — §2's scalar rule, retryable = 0.

Both map to their `wire::ErrorCategory` with `retryable = 0`; `kTxnConflict` remains the only retryable code.
*Done when:* every J2 form and every over-cardinality scalar returns its own code, never a generic `InvalidArgument`.

## 4. Grammar surface

**I7 — `CREATE TABLE ... WITH (key = value, …)`**, an extensible option **table**, not productions; unknown options are a parse error naming the option and its position. The v2 entry is `PHYSICAL_OPTIMIZER = ON|OFF`. **Waystone is deliberately not an option**: it is keyed on `(pattern_id, arg_hash)`, not on a relation. The storage form (`HEAP`/`BTREE`) stays a trailing clause; folding it in is a decision this item does not take.

**I8 — Session and admin statements.** `SET DURABILITY {STRICT|GROUP|RELAXED}`, `SHOW META`, `SHOW TABLES`, `SHOW PROFILE` (only in `KDS_PROFILING` builds). Ordinary statements returning ordinary result sets — one surface, one auth story. SET is excluded from fingerprinting.

**I9 — Join and nesting scope. `[AMENDED 2026-08-01 — the reversal]`**
v1 said "Nested structures — subqueries, CTEs, derived tables — are **out of scope**, not deferred". That is reversed for predicate position by J1: §2's four subquery forms are in scope, correlated included. Table-position nesting remains out and answers `Unsupported`.

The join surface: inner equi-join chains over 2+ relations (`JOIN … ON a.col = b.col`, chained and flat) with `AS` aliases. `LEFT`/`RIGHT`/`FULL`/`OUTER` are lexed and reserved but rejected `Unsupported` with a position, so clients get a truthful error and the grammar does not shift when they land.

**I10 — Filter scope.** Conjunctions of `col op {slot}`, `IN (list)`, `BETWEEN`, and the §2 subquery predicates. `OR`, `NOT` outside the reserved negation forms, and arbitrary expression trees are excluded — they blur classification, which is worth more than expressiveness here. `IN` list elements occupy slots, so `IN (1,2)` and `IN (?,?)` converge.

**I11 — `ORDER BY <col> [ASC|DESC] [, ...] LIMIT n [OFFSET m]`. `[AMENDED 2026-08-11 — general ORDER BY built, `docs/workplan-order-by.md`]`** Limit and offset are slots, so `LIMIT 10` and `LIMIT 20` share a `pattern_id` — corpus-pinned. A sort *column*, being an identifier, is part of the shape instead: `ORDER BY a` and `ORDER BY b` are different patterns. **Each clause is independently optional**: `LIMIT` without `ORDER BY` is well-defined here in a way general SQL cannot promise, because I12 already makes emission order a client contract, so the clause takes a prefix of an order the statement has. **The tail is the outermost non-aggregated block's**: over aggregated output it is `Unsupported` (fold order is not a contract; belongs with HAVING in `aggregate.md` §10's post-fold-consumer decision, which OB deliberately left open), and inside a subquery it is `Unsupported` with the byte. Which relation a sort column belongs to is the compile half's check, not the parser's. `LIMIT`'s execution is a sink-decorator quota over `RowSink`'s `kStop` — `exec::EmissionQuota`. The defining contract: the reply to `LIMIT n OFFSET m` is rows [m, m+n) of the unlimited reply. **`[AMENDED 2026-08-21 — the catalog views]`** That contract binds the *reply*, not the chain, so it holds over the one row source that is not a chain: a `sys.*` statement is answered by the catalog's typed readers before the compiler is asked for a chain, and the quota runs over its materialized rows after the `WHERE` and before the formatting — the same `EmissionQuota`, constructed from the clause rather than from a `StepChain`. `ORDER BY` there is `Unsupported` with the byte instead: `exec::OutputSort` normalizes its keys out of a `ChainFrame` against `SortKey`s resolved to indices in a Schema, and a view has neither. Before this the whole tail was parsed, accepted and silently dropped on a view, which is the failure this section exists to forbid.

Two claims this entry used to make are **superseded, and recorded rather than overwritten**. *"Non-pk ordering is `Unsupported` — the semi-sorted heap and the B+ tree make pk order the free order"*: pk order is still the free order, and it is now the order the compiler **elides** rather than the only one it serves — any other order costs an output sort, and the statement pays for it rather than being refused. *"`DESC` parses in no production and answers `Unsupported` — every chain links forward only"*: true of an executor whose only ordering mechanism was a walk. An output sort does not walk, so descending costs a comparator's sign; the refusal was retired because its reason expired, which is the standard this entry set when it kept the word in the grammar.

**I13 — Dialect compatibility is a non-goal.** No PG/MySQL emulation, no quoting mimicry, no shims. Compatibility requests are product decisions, not parser patches.

## 5. Execution contract

**I12 — Written order is the plan ("the statement is the chain").**
Execution order is textual order: the chain runs front to back, nested-looping into each subsequent step. A *documented client contract* — deterministic, predictable, appliance-appropriate — not a limitation to apologize for. An uncorrelated sub-chain is hoisted and runs once before the outer chain; a correlated one runs per outer row, where it is written.

**Decorrelation rewrites are forbidden**, not merely unimplemented: turning a correlated subquery into a semi-join, or hoisting its inner side into a read the written order never scheduled, is exactly the silent reordering this contract rules out, and it is not validatable against the trail model. `[NARROWED 2026-08-19]` This clause used to say "hashing its inner side once"; what it forbids is the *hoist* — reading the inner relation before written order reaches it. The statement-local inner build (sanctioned below) hashes inside the very walk written order schedules, which is why it is not this rewrite. The one permitted plan-level shortcut, because it changes no result and no order: a false uncorrelated top-level `EXISTS` short-circuits the statement without opening the outer relation.

**`ORDER BY` is the one sanctioned reordering, and it refines this contract rather than replacing it. `[AMENDED 2026-08-11 — OB4]`** A sort is a sink decorator: the chain still runs front to back and still emits in written order, and the reordering happens strictly downstream of every step, every trail entry and every access count. Arrival order is the sort's last key and always ascending, so **rows the clause does not distinguish come back in the order this contract gives them** — a client that orders by a column with ties still sees written order within each tie. What a sort does *not* do is license the engine to reorder anything the statement did not ask to reorder; the prohibition above is unchanged.
*Done when:* the contract is stated in the client manual; execution order provably matches written order for both orderings of one query.

**Equality propagation is the one sanctioned predicate rewrite, and it adds conjuncts, never removes, rewrites or reorders one. `[AMENDED 2026-08-18]`** From `A = B` — two columns of one chain — and `B = <literal>`, the compiler appends the implied `A = <literal>` to the flat conjunct list before attachment, so the step owning `A` can be *keyed* on it: a join whose restriction is written against the other relation stops compiling the keyed side to a full walk per outer row. The shape and the price are `bench/results-scenario3-library.md` §9's — a 10,086-page scan for six rows that the same engine's index answers in 7 pages when the predicate is written on one relation. Its constraints, each load-bearing:

- **Results are unchanged by transitivity.** Joins are inner-only (I9) and no `NULL` exists to make `=` non-transitive; the derived conjunct is *appended*, so the residual still carries every written conjunct and any step downgraded to a scan filters to identical rows — V14's own argument, extended by one implied conjunct.
- **Plans may only be strengthened.** Derived conjuncts sit after every written one, and the pk-equality choice takes the first usable equality in residual order — so a written key is never displaced by a derived one. A derived conjunct *can* do more than fill a scan: it may promote a written `kRange` to a `kLookup` (a strictly stronger trust class under invariant 9), change which secondary index a step enters, reach a `CabinProbe`, or flip a statement's class to `kPointSelect` when it proves the driving step a point. Every one of those is a stronger access to the same rows; none weakens one. (The `CabinProbe` case inherits that structure's measured cost inversion, §7 of the same bench file — a Cabin problem, not a propagation rule.)
- **At most one conjunct is derived per column**, and only for a column carrying no written equality-to-literal of its own — the first descriptor-matching literal in its class, in written order. A second literal on one column is plan-inert (a keyed candidate already exists) and result-inert (the written conjuncts fully express the predicate, contradiction included); without the bound, a class of *M* columns carrying *L* literals appends `L·(M−1)` conjuncts, which is a compile-time and per-row blowup an adversarial statement can drive to seconds.
- **Still `f(shape, catalog)`.** First-seen order everywhere, no data consulted; `docs/spec/index.md` §9's purity claim is unchanged.
- **The literal crosses only an identical type descriptor** (`type_val` and `len` both equal): it was coerced against the column it was written on and is copied bytes-for-bytes — re-coercing a coerced decimal would rescale it twice. A mixed-descriptor join key keeps its unpropagated plan.
- **`$param` propagates**, for the reason the lookup path treats one as pk-eligible: a declared pattern's body must compile to the plan the traffic's literal form takes.
- **Derived conjuncts are marked** (`StepPredicate::derived`): `ANALYZE` prints `derived` on their filter lines, and `CREATE PATTERN`'s parameter checks skip them — a warning or refusal must name a predicate the client can find in their text, and the derived occurrence's verdict is never different, since the descriptor guard makes both columns the same type.

Out of scope by decision: deriving column-column conjuncts (placement already keys on the earliest available side) and propagating range bounds through a join key. What this rewrite deliberately does not touch is order: the chain still runs front to back in written order, and the written order still decides which relation drives — propagation makes both writings of a restricted join fast, it does not choose between them.

**The statement-local inner build is the one sanctioned execution-time structure, and it adds a way to *locate* an inner match set, never a reorder of a row or a read. `[AMENDED 2026-08-19 — spec ratified, `docs/spec/join-inner-build.md`]`** When a join's inner step would walk per outer row — no index, no Cabin, no literal to propagate — the executor lets the first outer row's inner walk double as the build of a statement-lifetime map from join-column values to matching rows, and every later outer row probes the map instead of walking. Three facts keep it inside I12, each load-bearing:

- **The outer relation still drives.** The build changes how a match set is located, never which relation iterates or what joins what — the claim IX17 and CB12 already ratified for correlated probes, applied to a structure whose lifetime is one statement.
- **Read scheduling is unchanged.** The inner relation is first read exactly when written order reads it: the build is that walk's side effect, the Recording pattern `docs/spec/cabin.md` §4 ratified.
- **Emission order is captured, not reconstructed.** Buckets append in walk order, so a probe replays each key's matches in the order the walk would have emitted them — for a named key and an issued one alike, since build order *is* the walk's order.

Hard rules ratified with it: never the outer side, never an emission reorder, never survives the statement, never feeds Waystone. What the prohibition above still forbids is the hoist — building before written order first reads the inner side — which is the semi-join/hash-join rewrite and is not what this paragraph licenses.

**I15 — Nested access rules. `[NEW]`**
A nested sub-chain step and a join probe both perform storage access *inside* an outer walk's callback. Today that is safe only by accident: nothing evicts, so a span into a page frame survives an arbitrary number of nested fetches. Buffer-pool frame reclamation is `[OPEN]` in `CLAUDE.md`, and the day it lands, a step VM written against bare spans reads freed memory in exactly the workload that touches the most pages.

- **R1 — decode before descending.** At every step, the row is decoded into that step's owning frame buffer before any nested access. No span into a page frame — a tuple payload, or a descent's carried-out leaf — may be live across a call that can fetch another page.
- **R2 — nested steps are read-only.** Every nested walk uses read access, enforced structurally: the nested driver has no parameter for it. This makes the Halloween problem unreachable rather than merely absent, and stops a read-only statement from dirtying frames.
- **R3 — recursion is bounded**, at compile *and* at execute: the §2 depth cap and a maximum relations-per-chain, each a named `constexpr` with its derivation. The only bound today is the chain-hop budget, which is a corruption guard, not a plan guard.
- **R4 — a walk must be stoppable.** *(Done, V03.)* `ChainVisit`/`BtreeVisit` took a callback returning `Status` and treated any non-ok return as statement failure, so a walk could not end early. `Exists` ("stop at the first qualifying row") is unimplementable against that, and so are `LIMIT` and any cost guard. The callback returns an outcome instead, and "stop" ends the walk successfully. **"Stop" is never encoded as a `Status`** — no cancellation code exists engine-side, deliberately, and control flow through the error channel makes "did it fail or did it finish?" unanswerable at the call site.

*Done when:* a debug guard trips if any page fetch happens while a page-frame span is registered live; a stopping visitor touches no page beyond the one it stopped on; both bounds are refused at each end.

**I16 — NULL and cardinality. `[NEW]`**
The engine is two-valued today: a comparison against NULL is false, and no *stored* value can be NULL at all (the row codec rejects it, pending a null-bitmap format), so the only reachable NULL is an inline literal. `NOT IN`'s standard semantics are three-valued and the dangerous half is exactly that: if the subquery result contains a NULL and the probe matches nothing, `x NOT IN (S)` is UNKNOWN, not TRUE, so implementing it as `!IN` is silently wrong the day NULLs become storable.

The evaluator therefore computes a tri-state and **collapses it to a boolean in exactly one place** — UNKNOWN becomes false at the conjunct, one function, one call site. Today UNKNOWN is unreachable from stored data, which makes this free; when the null bitmap lands, one function changes and no wrong answer shipped in between. This is a deliberate two-valued *collapse*, not full 3VL, which waits on the null-bitmap format.

## 6. Trail integration `[CONFIRMED policy]`

- **Recording (J5, n = 2):** the first execution of an instance `(pattern_id, arg_hash)` only counts; the second records. Sightings live in a bounded, core-local in-memory table; eviction merely restarts the count, which is a performance event. `sys.patterns.use_count` continues independently for retention.
- **Per-step recording:** `Lookup`/`Probe` steps append entries in execution order with their `step_id` and per-entry `rel_oid` — the existing 32-byte format, unchanged. `Exists` steps record the witnessing row only. `Range`/`Scan`/`NotExists` record nothing.
- **Replay** consults the instance's trail for entries with the step's `step_id` and applies `docs/spec/waystone-concpets.md` §2 per entry; any miss falls through to the authoritative path *for that step alone*. Search-class steps use the trail only as a prefetch batch.
- **`Exists` replay is positive-only**, and the asymmetry is the whole point: a validated witness *proves* non-emptiness, because presence has a witness. A missing or invalid witness proves nothing and the probe runs. A trail can never conclude absence.

**I17 — Rule 0: the probe key must be re-derived. `[NEW — correctness]`**
Before a trail entry for a `Probe` step may be trusted, the executor derives the probe key from the **current** producing row it has in hand and requires it to equal the entry's `pk`. A mismatch is a miss for that step alone.

Without it, replay is a wrong-answer generator, and no existing rule catches it. Suppose the producing row's join column was updated from 77 to 91 between recording and replay. The entry for pk 77 passes every check in §2 — `rel_oid` matches, the Keystone id at the recorded slot is 77, the epoch matches, MVCC says visible — because **every rule validates the trail against storage and none of them looks at the query**. `UPDATE` overwrites in place and keeps `(page_id, slot)`, so nothing about the producing row looks stale. The join emits row 77; the correct answer is row 91.

The check is free at runtime: the producing row is already decoded by R1 and the probe key is already a resolved `ColumnRef`. It is mandatory before any `Probe` replay ships, it lives in **one** function shared by every replay path, and it requires a `docs/spec/waystone-concpets.md` §2 amendment plus a §11 test — update the producing row's key column between record and replay and require the *new* row.

Also to be recorded there: §2's epoch rule is a permanently-passing stub until per-page epoch storage is decided (`[OPEN]` in `CLAUDE.md`), so nothing may claim epoch validation as tested.

## 7. Executor `[PROPOSED]`

A small step VM in `src/exec/`: compile once (AST → `StepChain`, alongside class tagging), then iterate — a linear loop over steps, a nested loop for sub-chain steps, run to completion on the owning core. Cursor and row state live in the chain frame: no allocation per row, bounded by depth cap × per-step state. The VM is the compile contract J4 preserves — when the blueprint parser lands it emits the same `StepChain` and nothing downstream changes.

Two things the VM inherits from I15 rather than choosing: frames own their decoded rows (R1), and nested steps are read-only (R2). One thing it must not do: hold a `TableAccess` pointer across anything that can bump the catalog version.

**Cost.** Nothing suspends mid-statement on a cooperative core, so an unbounded correlated scan is a denial of service on a shared engine. A per-statement row-touch budget (named `constexpr` plus a config key) stops the walk through R4 and fails the statement with a clear status; failing is the kinder answer. A one-entry memo on the last probe key exploits the outer chain's pk order and must be provably result-neutral.

## 8. Open decisions — do not assume

**~~I14 — Aggregates (`COUNT`/`SUM`, `GROUP BY`)~~. RESOLVED 2026-08-06 by `docs/spec/aggregate.md`, and by that document only.** Neither of the two options this item offered was taken: aggregates are neither excluded from the grammar nor reserved-and-rejected. They are **built** (AG1-AG15), as a fold outside the executor that wraps the statement's row sink, so the compiled chain is byte-identical to the same statement without it.

What that resolution preserves, and why this item could be closed without disturbing anything above: **nothing is reserved**, so `COUNT` and `GROUP` are still identifiers that a column may be named after - a function head is an unqualified name from the set *followed by* `(`, and no production puts a paren after a column reference. Every previously accepted statement therefore lexes to the same token stream, `kFingerprintVersion` did not move, and no stored `pattern_id` or recorded waystone changed meaning. The golden corpus is the evidence.

J2's requirement stands and is now permanent for v1 rather than blocked: a subquery containing an aggregate or `GROUP BY` answers `Unsupported` with an exact position (AG8), because a fold inside a sub-chain puts an aggregation boundary where the execution model has none. `HAVING`, `AVG` and `ORDER BY` over aggregated output are refused the same way.

Also open:

- Depth cap default (4 `[PROPOSED]`); instance-sighting table size; `IN`-list and hoisted-set size caps; per-statement slot-table cap, which must also decide whether `kOuterRef` slots count against it.
- Subqueries in `INSERT`/`UPDATE` value position.
- Trail entry semantics for multi-row `Probe` fan-out beyond one page (spills onto `next_page_id`; the per-instance cap is inherited from `waystone-concpets.md` §9 retention).
- Ratification of the `[PROPOSED]` class list; whether `kUnclassified` is permitted in production builds or gated.
- Inherited: buffer-pool frame reclamation (R1 exists to survive it), per-page epoch storage and width (I17's stub), Waystone retention and eviction.

**Not a decision, a gap worth stating:** transactions are specified and not built, so a chain reads N relations with no snapshot and they can in principle be mutually inconsistent. Unobservable today — one cooperative thread, no concurrent writer mid-statement — and considerably more visible once chains exist. That is `docs/spec/txn.md`'s work, not this document's.

## 9. Testing requirements

1. **Oracle equivalence:** every step-chain statement checked against a naive reference evaluator over small fixtures — joins, each subquery form, NULL-in-`NOT IN`, cardinality violations.
2. **Advisory family:** results byte-identical with Waystone off, trails dropped mid-run, and recording disabled — now across nested chains too.
3. **Replay rules:** §1's per-kind table enforced by instrumentation — a `NotExists` step provably never reads a trail; an `Exists` step never concludes absence from one; rule 0's stale-probe-key case returns the new row.
4. **n = 2:** first execution records nothing (instrumented); second records; sighting-table eviction restarts the count with no correctness change.
5. **`Unsupported` surfaces:** every J2 form errors with an exact position, and nothing falls through to a generic path.
6. **Depth and numbering:** chains at the cap; global `step_id` stability across executions of one instance.
7. **Fingerprint invariance:** every pre-existing corpus statement's `pattern_id` is unchanged by every grammar addition; convergence of inline and bound forms; a correlated statement's `arg_hash` stable across outer rows.
8. **Execution equivalence:** every case runs heap×heap, heap×btree and btree×btree with identical rows in identical order; `Probe` and `Scan` strategies agree row-for-row; execution order matches written order for both orderings of one query.
9. **Nested access:** the R1 guard trips when deliberately violated; a stopping visitor touches no page past the stop; a false uncorrelated `EXISTS` opens zero pages of the outer relation; a correlated `EXISTS` short-circuits, proven by page-touch count.
10. **Grammar:** golden parse trees for every production; byte- and token-level fuzzing — never crashes, always returns `Status`; a corpus of nesting attempts outside predicate position contains no accepted parse.
11. **Zero-alloc/zero-copy** (phase V-6): instrumented lexer and warm parse; interleaved conjunct emission; escaped views caught in debug builds. **Partly done 2026-08-03**: token text is a view into the statement and lexing allocates nothing (`include/kds/parser/token.hpp`), and the fingerprint is accumulated during the parse rather than by a second lex. Still open here: the arena, the flat AST, and the debug-build check for escaped views — today the "a token must not outlive its SQL" rule is documented and structurally respected (tokens never leave a parse), not enforced.

## 10. Required amendments (gate)

1. `docs/parser.md` and `docs/step-chains.md` (and `docs/parser-workplan.md`, `step-chains-workplan.md`) — superseded banners pointing here.
2. **Done (V02).** `include/kds/base/status.hpp` — `kUnsupported` and `CardinalityViolation`; correct the note in `include/kds/wire/kwp.hpp`. `wire::ErrorCategory` gained `kCardinalityViolation` with them, appended: `docs/spec/protocol.md` §11 has categories mirroring engine `Status` over an open-ended list, and a client cannot fix a cardinality violation by fixing its arguments.
3. **Done (V03).** Storage walk contract — `ChainVisit`/`BtreeVisit` callbacks return an outcome, not a bare `Status` (I15 R4).
4. `docs/spec/waystone-concpets.md` — §2 gains the negation rule and **rule 0**; §7 gains the subquery cases; §9's recording policy moves to `[CONFIRMED] n = 2`; §11 gains the stale-probe-key test and the note that the epoch rule is a stub.
5. `docs/spec/client-manual.md` — subquery forms, depth cap, `NOT IN` NULL semantics, the written-order contract, and the `Unsupported` surfaces.
6. `CLAUDE.md` — parser and Waystone summary lines, the documents list, and the open list.

## Addendum — `DELETE` (added by `txn-workplan.md` T10, 2026-08-04)

`DELETE FROM <t> [WHERE <cond> [AND <cond>]*]` is a statement head. It was
added by the transaction work rather than by a parser task, because
`docs/spec/txn.md` §10 tests it and the delete-mark half of the visibility
predicate is unreachable without it.

What it does and does not change:

- **The WHERE is the same production `UPDATE` uses** (`ParseOptionalWhere`
  at depth 0), compiled through the same `exec::CompileWhere`, so a
  `DELETE`'s predicate means exactly what the `SELECT` that found the rows
  meant. Predicate-position subqueries nest exactly as a `SELECT`'s do.
- **No fingerprint bump.** `fingerprint.hpp`'s rule names this exact case:
  making a statement fingerprintable that previously was not does not move
  any hash already stored. `DELETE` is not fingerprinted today and the
  golden corpus records it as `ok  -  -`.
- **`DELETE` is not a reserved word.** Like `SELECT`, `INSERT` and
  `UPDATE`, it is matched by text where the grammar expects it, so a column
  may still be named `delete`.
- It does not compile to a step chain. `HandleDelete` walks the relation
  itself, exactly as `HandleUpdate` does, and applies the same visibility
  predicate through `txn::Classify`.
