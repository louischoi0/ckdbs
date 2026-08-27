# Query Language v2 — Workplan

Work instructions, companion to `parser-v2.md`. **Supersedes `docs/parser-workplan.md`** (`PR01`-`PR24`) and **`step-chains-workplan.md`** (`SC01`-`SC10`); both are kept as history, and the disposition of every one of their tasks is at the end of this file.

Execution rules:
- Do tasks in numeric order unless "Needs" says otherwise.
- Each task ships with its listed tests in the same change; `bash test.sh` green is part of "done".
- Touching an `[OPEN]` item in the spec or in `CLAUDE.md` means **stop and flag** — the known collisions are tabled at the end.
- `V01`'s golden corpus is regression-mandatory from the moment it exists, and the advisory-contract family from `V21` onward.

**Numbering.** `V##`. A third prefix is deliberate: `CLAUDE.md` already warns that the Waystone and protocol workplans share `P01`-`P17` and that a bare number is ambiguous; `PR*` and `SC*` stay reserved for the two superseded files so the disposition tables keep meaning. Cite the file, not the bare number.

**Vehicle (J4).** Phases V-2 through V-5 extend the **current** recursive-descent parser and its owning AST. The blueprint parser — arena, flat AST, slot table, binding at PARSE — lands afterwards, in phase V-6, and its acceptance criterion is that it emits **identical `StepChain`s** over V23's corpus.

**Two of phase V-6's items landed early, out of Waystone's cost work (2026-08-03)**, because replay's overhead turned out to be parser overhead: **fingerprinting is folded into the parse pass** (the lexer feeds a `FingerprintAccumulator`, so nothing lexes twice and `kFingerprintVersion` did not move — a differential test compares both paths over the whole corpus), and **tokens are zero-copy** (`Token::text` is a `std::string_view` into the statement, so lexing allocates nothing and a `Token` is trivially copyable). The owning AST is unchanged: it still copies every name it keeps, which is I4's copy-at-the-boundary rule and what lets a `Statement` outlive the SQL. Measured at 3-8% off every statement's parse, and it took Waystone's B+ tree overhead from 13-15% to 3-7% (`bench/results-waystone-v2.md`). The `Compile(AST) → StepChain` contract (V14) is the seam that makes that a checkable statement rather than a hope.

**Baseline.** Recursive descent over a `std::string`/`std::vector` AST, a copying lexer, no class tag, no binding. Grammar: `CREATE TABLE` / `INSERT` / `SELECT *` / `UPDATE`, AND-only `col op literal`. Fingerprinting exists as a separate pass over the same lexer and stays that way until V29. There is no join execution anywhere: the scan primitive takes exactly one relation, and its callback cannot stop it.

---

## Phase V-0 — the gate

**V00 — Document merge and amendments.** — **done.**
Files: `docs/spec/parser-v2.md`, this file, superseded banners on `docs/parser.md`, `docs/parser-workplan.md`, `docs/step-chains.md`, `step-chains-workplan.md`; `CLAUDE.md`; `docs/spec/waystone-concpets.md`; `docs/spec/client-manual.md`.
Two specs existed for one subject and contradicted each other on classification, vehicle, fingerprint versioning, `EXISTS` replay and recording policy. One document now carries J1-J5 and the architecture items together. Spec §10's amendment list is this task's checklist; the waystone and client-manual edits ride with the tasks that make them true (V17, V20, V22) rather than being claimed up front.
Done when: grep finds no "out of scope, not deferred" claim for predicate subqueries, and no document except the two banners tells a reader to build from `parser.md` or `step-chains.md`.
Needs: nothing.

---

## Phase V-1 — engine prerequisites

Goal: the three things the language needs that neither source spec named. None of them changes the accepted language by one byte, which is what makes them safe to do first.

**V01 — Golden-corpus lock-in.** *(= PR01)* — **done.**
Files: `tests/parser_golden_test.cpp`, `tests/testdata/parser_corpus.txt`.
Capture today's accepted/rejected behaviour of every production before touching code, plus each statement's `pattern_id`. The corpus is the regression oracle for every phase, and the `pattern_id` column is what proves the "additive, no version bump" claim in spec I1 rather than assuming it.
Done when: every production and error path in `src/parser/parser.cpp` is covered; any shape change fails it.
Needs: nothing.

**V02 — `kUnsupported` and `CardinalityViolation`.** *(spec I18)* — **done.**
Files: `include/kds/base/status.hpp`, the stale note in `include/kds/wire/kwp.hpp`, `tests/status_test.cpp`.
J2's whole surface promises `Unsupported` and no such code exists; §2's scalar rule needs `CardinalityViolation`. Append both — the enum's own comment says appending is free, since nothing persists a `StatusCode` — and map them to their wire categories with `retryable = 0`.
Done when: `kTxnConflict` is still the only retryable code; the `kwp.hpp` note is corrected.
Needs: nothing. **Blocks V05-V12.**

**V03 — Stoppable walks.** *(spec I15 R4)* — **done.** The visitor returns `StatusOr<storage::VisitControl>` (`include/kds/storage/visit.hpp`). One thing the task did not anticipate: `StatusOr<T>` built from `Status::OK()` holds no value, so the `return Status::OK();` every visitor said before this change would have made `value()` undefined rather than failing. `StatusOr::has_value()` plus `storage::ResolveVisit()` turn that port mistake into a reported `InvalidArgument`.
Files: `include/kds/storage/heap/heap_chain.hpp`, `src/storage/heap/heap_chain.cpp`, `include/kds/storage/btree/btree.hpp`, `src/storage/btree/btree.cpp`, both dispatcher call sites, `tests/heap_chain_test.cpp`, `tests/btree_test.cpp`.
The visitor returns an outcome; "stop" ends the walk with `Status::OK()`. Two declarations, two loop bodies, two call sites. Without it `Exists` cannot short-circuit, `LIMIT` cannot be implemented, and no cost guard can fire — all three would have to abuse `Status` as control flow, which is how "did it fail or did it finish?" becomes unanswerable at the call site.
Tests: stop mid-page, at a page boundary, on the last page; an error still aborts.
Needs: nothing. **Blocks V17-V19.**

*Gate:* corpus green, both codes reachable, walks stoppable, dispatcher behaviour unchanged.

---

## Phase V-2 — grammar, bolted onto the current parser

**V04 — Lexer: keywords, dot, byte offsets.** *(= SC02, amended)* — **done.** The seven reserved words share one `TokenType::kKeyword` carrying a `Keyword` enum rather than getting a token type each: the fingerprint must hash a keyword exactly as it hashes an identifier, and one token type makes that one line in `ShapeTagOf()` that every later-reserved word inherits. `kFingerprintVersion` stays at 1; only the three dot-bearing corpus lines moved, all `-` → hash. "Sub-select paren handling" needed no lexer change — parens already lex; nesting is V07's, in the parser.
Files: `include/kds/parser/token.hpp`, `src/parser/lexer.cpp`, `tests/lexer_test.cpp`.
`JOIN`, `ON`, `AS`, `IN`, `EXISTS`, `NOT`, `BETWEEN` recognized as keywords; a `.` token (it lexes as an error today, so `a.x` cannot be tokenized at all); sub-select paren handling; `byte_offset`/`length` on every token, which is what makes J2's "exact position" possible. Integer tokens keep the raw digit view alongside the decoded value — a signed decode cannot judge the full unsigned pk range.
Done when: every pre-existing corpus statement's `pattern_id` is **unchanged** (additive shape, no `kFingerprintVersion` bump).
Tests: lexing goldens; `a.x` lexes as three tokens; fingerprint invariance over V01's corpus.
Needs: V01.

**V05 — Joins and aliases.** *(= SC03, amended)* — **done.** `SelectStmt::table_name` is gone, replaced by `from` + `joins` in written order; `RelationRef::binding()` is the alias-or-name every uniqueness check runs on. Two decisions the task did not settle: an unqualified ON column is `Unsupported` (well-formed SQL whose resolution is V14's, not a typo), and `INNER` is left unreserved so `INNER JOIN` is trailing garbage — spec I9 names only the outer keywords. `HandleSelect` refuses a multi-relation statement rather than scanning `from` and ignoring the rest, which would answer a two-relation question with one relation's rows. The `SELECT * FROM t JOIN u …` corpus line is expected to flip once more at V06.
Files: `src/parser/parser.cpp`, `include/kds/parser/ast.hpp`, `tests/parser_join_test.cpp`.
`FROM a JOIN b ON a.col = b.col [JOIN c ON …]`, flat and chained, with `FROM t AS a` aliases. Outer-join keywords lex and answer `Unsupported` with a position. A FROM list naming one relation twice without distinct aliases is `Unsupported`, not a silent ambiguity — which is also what makes a self-join expressible rather than accidentally wrong.
Tests: parse goldens; written order preserved in the AST; a self-join parses; outer keywords' truthful errors.
Needs: V02, V04.

**V06 — Projection and qualified names.** — **done.** `QualifiedColumn` became `ColumnName` with an *optional* qualifier — named so because `ColumnRef` belongs to V14's compiled `{up, rel_slot, col_pos}` form, and the difference between them is the resolution step. `Condition::col_name` (a bare string) became `Condition::col`, so WHERE takes `a.x` too; `Assignment::col_name` did not, since UPDATE reads one relation and a qualifier there says nothing. The star rule is checked **after** the join and binding rules on purpose: "outer joins are not supported" and "t is named twice" tell a client what to do, where "SELECT * is ambiguous" would send them to fix the wrong thing first. A bare column in a multi-relation select list is *accepted* and left for V14 — the spec's resolution rule needs a catalog, and refusing here would forbid a form the language allows. The dispatcher refuses an explicit list rather than emitting every column, and checks any WHERE qualifier against its one binding: skipping that would let `WHERE u.id = 1` silently filter on `t`'s `id`.
Files: `src/parser/parser.cpp`, `tests/parser_projection_test.cpp`.
An explicit select list — `SELECT a.x, b.y` — becomes available and, for multi-relation statements, mandatory: `SELECT *` is ambiguous across relations. Projection shape must never affect the statement class.
Needs: V05.

**V07 — Predicate-position subqueries.** *(= SC04)* — **done except the aggregate bullet, which I14 blocks.** All four forms parse into `Condition::kind` (`PredicateKind`) plus a `shared_ptr<SelectStmt>` — shared rather than unique because `Statement` is copied by value and `unique_ptr` would make the whole AST move-only; safe since the AST is immutable after parse. `NOT IN` is its own kind, not `IN` with a flag: it compiles to `NotExists` (search-class, never replayable) where `IN` compiles to `Exists`, so the AST must keep them apart for that to stay decidable. Depth is a **parameter, not parser state** — it has to unwind exactly with the recursion, and a member would need restoring by hand on every error path; a test pins that sibling subqueries each get the full budget. `ParseRelationRef` refuses a `(` outright, which is the structural rule ("the relation-reference production must never reach the statement production") and covers FROM *and* join position from one place; `WITH` is answered by name at the statement head. A nested block does not consume a `;`.

**The gap, decided 2026-08-01:** a subquery containing `COUNT(*)` or `GROUP BY` still gives a bare syntax error, not `Unsupported` with a position. Closing it means either excluding aggregates from the grammar or reserving-and-rejecting them — which *is* I14, an open decision whose own text says "do not implement either path". Two corpus lines record the current behaviour and name I14 as the blocker. Revisit with I14, not before.
Files: `src/parser/parser.cpp`, `tests/parser_subquery_test.cpp`.
Scalar comparison against `(SELECT …)`; `IN`/`NOT IN (SELECT …)`; `EXISTS`/`NOT EXISTS (SELECT …)`; recursion to the depth cap with a counted guard. A nested SELECT is admissible in exactly these positions: the relation-reference production must not reach the statement production, and `WITH` must not lex as a statement head.
Done when: every J2 form — FROM-position nesting, CTEs, aggregates inside, over-depth — answers `Unsupported` with an exact position, never a bare syntax error and never an accepted parse.
Tests: parse goldens per form; depth-cap boundary (at the cap parses, one deeper errors); a corpus of nesting attempts outside predicate position.
Needs: V02, V05.

**V08 — Predicate surface: `IN (list)`, `BETWEEN`.** *(= PR14)* — **`BETWEEN` done 2026-08-03; `IN (list)` still open.**
Files: `src/parser/parser.cpp`, `src/exec/step_compiler.cpp`, `tests/step_compile_test.cpp`, `tests/range_scan_test.cpp`.
Flat conjunct list, no expression tree. `OR`, `NOT` outside the reserved negation forms, and parenthesized expressions answer `Unsupported` with a position.

`col BETWEEN <low> AND <high>` parses as `PredicateKind::kBetween` and **lowers to its two ordinary conjuncts** in the compiled chain; the pk range it may also put on the step is a hint on top of them, never a replacement, which is what keeps "downgrading any step to a plain scan cannot change the result" true. **No `pattern_id` moved**: `BETWEEN` has lexed as a reserved keyword since V04, so becoming parseable changes no shape stream — the corpus line kept its hash while its verdict flipped, which is the whole reason V04 reserved it early.

`IN (list)` is untouched and still reports through `ParseSubquery`.
Needs: V02, V07.

**V09 — `ORDER BY` pk + `LIMIT`/`OFFSET`.** *(= PR15)* — **done 2026-08-10, parse and execution both. `[SUPERSEDED IN PART 2026-08-11 — docs/workplan-order-by.md]`** The `LIMIT`/`OFFSET` half stands exactly as written below, including the fingerprint slot rule and `exec::EmissionQuota`. The `ORDER BY` half does not, and the entry is kept rather than rewritten so the reasoning is legible: **the clause is no longer discarded** — general ordering compiles resolved keys onto `StepChain::sort_keys` for a sink-side sort, and `DESC` and multi-key lists are accepted. The one form V09 described is now the one form the compiler *elides*, which preserves its zero-cost claim precisely. One correction the supersession forced: the quota now runs **downstream** of the sort on a sorted statement, so the "a filled quota stops the walk on the tuple that filled it" property below holds only for unsorted and pk-elided statements. The tail is `[ORDER BY <col> [ASC]] [LIMIT <n>] [OFFSET <m>]`, clause order fixed as I11 writes it and each clause independently optional — `LIMIT` without `ORDER BY` is deliberate, because emission order is already I12's client contract (written order across steps, pk order within one), so the clause takes a prefix of an order the statement has rather than one it hopes for. Nothing was reserved: all five words are ordinary identifiers matched by text at clause position, `kFingerprintVersion` did not move, and the two corpus lines that predated the task flipped verdict with their hashes intact — with **I11's slot rule now corpus-visible**: `LIMIT 10` and `LIMIT 20` share `pattern_id cfd92d5cf2657bf9` and differ in `arg_hash`. Refusals, each with a byte: `DESC` is `Unsupported` (every chain links forward only; the word stays in I11's grammar so the refusal does not shift when a reverse walk lands); the whole tail is `Unsupported` over an aggregated statement (groups emit in fold order, which is not a contract — joins `aggregate.md` §10's post-fold-consumer decision) and at subquery depth (AG8's shape); a count is a non-negative integer literal decoded **from its digits**, so a value past uint64 refuses rather than wraps — TY11's wrapped-literal lesson one layer down. What the parser deliberately does not decide: whether `order_by` names the driving relation's pk. Pk-ness is catalog knowledge, so `ORDER BY <pk> [ASC]` is the *compiler's* validated no-op — accepted only on the driving relation's pk, refused elsewhere at the stored byte — and the column is then **discarded**: the accepted form names the order the chain already emits, so no chain field carries it and nothing downstream can come to depend on one. **The execution half is `exec::EmissionQuota`** (`include/kds/exec/pagination.hpp`), a sink decorator at the dispatcher — AG1's seam, for AG1's reason — so the chain of a limited statement is its unlimited twin's bit for bit and `IsTrailReplayable` did not move; `kEmitThenStop` rides `RowSink`'s `kStop` (V03), so a filled quota stops the walk on the tuple that filled it, proved by ANALYZE's `pages=` in `tests/pagination_exec_test.cpp`. ANALYZE runs the quota too (AG15's reason one seam over) and `FormatPlan` prints a `quota` line after the projection. The defining contract, pinned end to end on heap and btree: **the reply to `LIMIT n OFFSET m` is rows [m, m+n) of the unlimited reply.** OFFSET's skipped rows are examined and still charge the row-touch budget — the quota bounds output, the budget bounds work.
Files: `src/parser/parser.cpp`, `include/kds/parser/ast.hpp`, `src/exec/step_compiler.cpp`, `include/kds/exec/pagination.hpp`, `src/server/command_dispatcher.cpp`, `tests/parser_pagination_test.cpp`, `tests/pagination_exec_test.cpp`.
Needs: V08.

**V10 — `DELETE`.** *(= PR13)*
Files: `src/parser/parser.cpp`, `tests/parser_test.cpp`.
`DELETE FROM <rel> WHERE <conjuncts>`, sharing UPDATE's predicate surface — the class list names `kPointDelete` and no production ever existed. Unrestricted `DELETE` with no `WHERE` ships **rejected** as `Unsupported`, flagged for ratification, rather than silently allowing a full-table delete.
Needs: V02, V08.

**V11 — `CREATE TABLE ... WITH (k = v)`.** *(= PR11)*
Files: `src/parser/parser.cpp`, `include/kds/parser/table_options.hpp`, `tests/parser_ddl_test.cpp`.
An option **table**, not productions; the v2 entry is `PHYSICAL_OPTIMIZER = ON|OFF`; unknown options are a parse error naming the option and its position. The flag round-trips into the catalog here rather than waiting — nothing consumes it yet, which the task states plainly.
Needs: V02.

**V12 — Session and admin statements.** *(= PR12)*
Files: `src/parser/parser.cpp`, `src/server/command_dispatcher.cpp`, `tests/parser_admin_test.cpp`.
`SET DURABILITY {STRICT|GROUP|RELAXED}` → `kSessionSet`; `SHOW META`, `SHOW TABLES` → `kAdminShow`; `SHOW PROFILE` only under `KDS_PROFILING`. SET is excluded from fingerprinting. Note there is no `SET` handler in the dispatcher at all today, despite the fingerprint tests already treating it as a session statement.
Needs: V02.

**V13 — Grammar hygiene and fuzz.** *(= PR18)* — **done, scoped to V04-V07.** 35k mutated inputs across seven properties: never crashes, always returns a Status with a message, the lexer terminates with every token inside its input, depth 5000 is refused rather than overflowing the stack, and the fingerprint stays a pure function agreeing with the parser about what lexes. The I13 hygiene check greps the parser sources for productions that exist only to accept another dialect's spelling. **Its gate is not met:** V08-V12 are unbuilt, so no input exercises `IN` value lists, `BETWEEN`, `ORDER BY`/`LIMIT`, `DELETE`, table options or session statements. Their forms belong in the seed corpus when they land.
Files: `tests/parser_fuzz_test.cpp`, spec §4.
Byte- and token-level fuzzing: never crashes, never allocates unboundedly, always returns `Status`. Golden parse trees for every production. No "for compatibility" production exists (grep-enforced).
Needs: V04-V12.

*Gate:* the whole language parses; every reserved or inexecutable form returns its own code with an exact position; fuzz clean; every pre-existing `pattern_id` unchanged.

---

## Phase V-3 — compile

**V14 — The step compiler.** *(= SC05, amended — the contract)* — **done.** `Compile(AST) → StepChain` in `src/exec/step_compiler.cpp`. Two things the task did not spell out. **Equality is symmetric**: `ON a.b_id = b.id` and `ON b.id = a.b_id` are the same join, so both orientations are examined — checking only the left side made which relation could probe depend on the order the client typed the sides in. And **the probe key is kept in the residual list as well**, which turns "a Probe and a Scan agree row-for-row" from an argument into a structural property: the residual list alone fully expresses the predicate, so downgrading any step to a scan cannot change the result. That is exactly what makes invariant 9's fall-through safe.
Files: `include/kds/exec/step_chain.hpp`, `src/exec/step_compiler.cpp`, `tests/step_compile_test.cpp`.
`Compile(AST) → StepChain`: resolve relations to oids and columns to `ColumnRef{up, rel_slot, col_pos}` against the catalog (spec I5 — under the bolt-on this is where name resolution lives); assign an access kind per spec §1; number steps globally in compile order; tag the class (`kJoinSelect` absorption, J3). A pure function of the AST — same statement, same chain, always.

Access-kind assignment is one decision with two consumers: it is the executor's probe strategy *and* Waystone's lookup/search line, deliberately not two implementations that can drift. A step is `Probe`/`Lookup` iff the equality binds against the relation's **first schema column** — the Keystone pk, the only column a lookup can address (invariant 11).

**Do not reuse `PkEqualityTarget`.** It refuses whenever the WHERE clause holds more than one condition, correctly for a point *statement* that cannot shortcut a second predicate — but a chain step only *locates* a candidate and every residual predicate is evaluated on the located row before it is accepted. Reusing it degrades every chain with a WHERE clause to a full scan per step. It also matches column names as strings, and a compiled reference carries no name.
Done when: a compiled chain contains no identifier on any execute path; two compiles of one statement are bit-identical.
Tests: chain goldens per statement form; numbering stability; purity; `JOIN b ON a.b_id = b.id` compiles `Probe` and `ON a.x = b.name` compiles `Scan`.
Needs: V13.

**V15 — Correlation analysis and sub-chain placement.** — **done.** Correlation is `does any reference inside point outward?` — structural, not a heuristic, so the placement is stable across executions and safe to record a trail against. **It exposed a grammar gap:** a correlated subquery is written `WHERE inner.col = outer.col`, and WHERE accepted only a literal on the right, so the form J1 put in scope was unspellable. Spec I10 says `col op {slot}`; §2 needs `col op col`; reconciled in favour of §2, which flips the corpus line `WHERE a = b` from InvalidArgument to ok. Also settled: a value-bearing subquery (`IN`, scalar) must project exactly one column — `*` has no single value to mean and picking the first would make an unrelated schema change alter the answer.
Files: `src/exec/step_compiler.cpp`, `tests/step_correlation_test.cpp`.
Classify each sub-chain uncorrelated (no reference with `up > 0`) → hoisted prefix, executed once; or correlated → nested step with its correlation columns bound as probe keys. Resolve `up` levels; unqualified names resolve innermost-first and stop at the first matching level.
Tests: an inner reference to an outer column resolves with the right `up`; adding a column to an outer relation cannot change an inner chain's meaning; an ambiguous unqualified name errors with its byte position; hoisting decisions pinned per form.
Needs: V14.

---

## Phase V-4 — execute

**V16 — Row frames and index-based evaluation.** — **done.** `DecodeRowInto` writes into caller-owned slots and `DecodeRow` is now a wrapper over it, so there is one decoder. `ChainFrame` is one values buffer plus per-step base offsets, sized once per statement — a `ColumnRef` resolves in one add and a scan allocates O(1). The name-matching evaluator is **deleted**, and `CompilePredicates` exists so UPDATE resolves its WHERE through the same path rather than keeping a second evaluator alive: two evaluators are two answers to "does this row qualify".
Files: `include/kds/exec/row_codec.hpp`, `src/exec/row_codec.cpp`, `include/kds/exec/chain_frame.hpp`, `tests/row_codec_test.cpp`.
`DecodeRowInto(schema, payload, out, base)` writing into a caller-owned buffer, with the existing `DecodeRow` kept as a wrapper so no current caller changes. A chain frame is one values buffer plus per-step base offsets, so a `ColumnRef` resolves in one add. Predicate evaluation moves from name matching to `ColumnRef` indexing, and **the name-matching evaluator is deleted rather than kept alongside**: in a multi-relation world it is silently wrong — an unknown column returns "no match" instead of an error, which drops every row rather than failing.
Done when: a scan of *n* rows performs O(1) allocations, not O(n).
Tests: byte-identical output against the old evaluator over V01's corpus.
Needs: V14.

**V17 — Step VM: linear chains.** *(= SC06)* — **done. Joins and projection now return rows.** `src/exec/step_vm.cpp`; the dispatcher is parse → compile → execute and its V05/V06/V07 refusals are gone rather than kept, along with `CheckWhereQualifiers` — the compiler resolves the same clause and gives the same answers. R1 is enforced by a thread-local page-span guard: a step registers its span, decodes, and **releases before descending**, and any fetch made while a span is registered trips it. Today no store evicts, so the span stays valid by accident — which is the kind of safety that stops holding silently. Tests pin both equivalences: heap×btree in all four combinations return identical rows in identical order, and a chain forced down the Scan path matches the Probe path row-for-row.
Files: `include/kds/exec/step_vm.hpp`, `src/exec/step_vm.cpp`, `src/server/command_dispatcher.cpp`, `tests/exec_chain_test.cpp`.
Linear iteration over steps with a nested loop per subsequent relation, enforcing **R1** (decode before descending), **R2** (nested steps read-only, structurally) and **R3** (bounds at execute as well as compile). `Lookup`/`Probe` go through the pk descent; `Scan`/`Range` through the relation walk. `LIMIT` becomes implementable here, on V03. The dispatcher's hand-rolled first-token split gives way to parse → compile → switch.
Done when: execution order provably matches written order; a debug guard trips if any page fetch happens while a page-frame span is registered live; grep finds no shape analysis in the executor.
Tests: 2- and 3-relation chains across heap×heap, heap×btree, btree×btree with identical rows in identical order; `Probe` and `Scan` strategies agree row-for-row; the R1 guard fires when deliberately violated.
Needs: V03, V15, V16.

**V18 — Step VM: nested chains, negation, cardinality.** *(= SC07, amended)* — **done. The language runs.** Tri-state with `Collapse()` as the single collapse point (I16), so `NOT IN` is not `!IN` and will stay correct when NULLs become storable. Scalar cardinality reads a second row and stops — enough to know, and no first-row pick. **V15's hoisting model was wrong and V18's tests found it:** an uncorrelated `IN` cannot be evaluated once, because its *set* is row-independent but the comparison against each outer row is not — spec §2 says "hoisted probe **set**", a materialized set plus a per-row test. Only `EXISTS`/`NOT EXISTS`, which have no outer column, leave the row loop; value-bearing forms attach to the step holding their outer column and re-run per row until the set is materialized. `ExecStats` makes the work-avoidance claims checkable: a false uncorrelated `EXISTS` opens exactly one relation, and `EXISTS` over a 20-row relation examines one row. UPDATE gained `CompileWhere` + `EvaluateConjuncts` so its WHERE means what a SELECT's does — a second evaluator would drift on the first NULL.
Files: `src/exec/step_vm.cpp`, `tests/exec_subquery_test.cpp`.
Hoisted prefix sub-chains executed once; correlated sub-chains executed per outer row with correlation values read through the frame stack and **never written into the AST**. `Exists` short-circuits on V03. `NotExists` is the same walk, negated. Tri-state `IN`/`NOT IN` with the single collapse point (spec I16). Scalar cardinality: zero rows → NULL, more than one → `CardinalityViolation`. A false uncorrelated top-level `EXISTS` short-circuits the whole statement without opening the outer relation.
Tests: each form correlated and uncorrelated; NULL in a `NOT IN` result makes the predicate never-true; a two-row scalar subquery fails with its own code; short-circuit proven by page-touch count; a false uncorrelated `EXISTS` opens zero pages; nesting at the depth cap.
Needs: V17.

**V19 — Cost guards and meters.** — **done.** `exec::Budget` charges one decoded tuple per row across every step and sub-chain, and refuses with a new `StatusCode::kResourceExhausted` (not retryable — re-running does the same work and stops in the same place). **Per statement, not per chain**, shared by reference into every sub-chain: a per-chain budget would let a correlated subquery spend the full allowance once per outer row, which is exactly the shape being bounded. Counted in tuples rather than wall clock on purpose — a timer makes a statement's success depend on what else the machine was doing, so the same statement on the same data would pass and fail at random. Config key `max_rows_touched` (0 = unlimited), default `100'000'000` `[PROPOSED]`. The one-entry probe memo caches a **location**, not a row, so a hit re-reads and re-filters exactly what a fresh descent would hand to the same code — which is what makes "identical with the memo on and off" structural rather than a property of the test data. Meters: `probe_memo_hits`, `correlated_scans`.
Files: `src/exec/budget.cpp`, `kds.conf.sample`, `tests/exec_budget_test.cpp`.
A per-statement row-touch budget (named `constexpr` plus a config key, precedence per `kds.conf.sample`) enforced through V03's stop; a one-entry memo on the last probe key, exploiting the outer chain's pk order; a correlated-scan counter. Nothing suspends mid-statement on a cooperative core, so an unbounded correlated scan is a denial of service on a shared engine — failing it is the kinder answer.
Tests: a runaway correlated scan fails with a clear status instead of hanging; results byte-identical with the memo on and off.
Needs: V18.

**V20 — Written-order contract, documented and tested.**
Files: `docs/spec/client-manual.md`, `tests/exec_order_test.cpp`.
The client manual gains the written-order contract, the subquery forms, the depth cap, `NOT IN`'s NULL semantics and the `Unsupported` surfaces (spec §10 items 5).
Done when: reordering the FROM list provably reorders execution; no decorrelation rewrite exists (grep + test). **Amended 2026-08-20** (`docs/spec/join-inner-build.md`, sanctioned in `docs/spec/parser-v2.md` §5): the statement-local inner build hashes a walked join's inner side once per statement, which is a *structure* the same correlated probe reaches — the outer relation still drives, emission order is unchanged, and no statement is rewritten. The test's subject is therefore "no statement is rewritten and no algorithm is chosen", not "nothing hashes".
Needs: V17.

*Gate:* the language runs; results identical across storage forms and across probe strategies; no statement is unbounded.

---

## Phase V-5 — the trail

**V21 — Trail recorder, n = 2.** *(= SC08)*
Files: `src/stats/recorder.cpp`, `src/exec/step_vm.cpp`, `tests/waystone_record_test.cpp`.
A bounded core-local sighting table; the first execution of an instance counts, the second records. `Lookup`/`Probe` steps append entries in execution order with their global `step_id` and per-entry `rel_oid`; `Exists` records the witnessing row only; `Range`/`Scan`/`NotExists` record nothing. The executor emits `(rel_oid, pk, page_id, slot, epoch, step_id)` **as it goes** — re-deriving it afterwards is the search the trail exists to avoid. Never on a failure path.
Tests: first execution records nothing (instrumented), second records; sighting-table eviction restarts the count with no correctness change; a failing statement records nothing.
Needs: V18, waystone P08.

**V22 — Replay, and rule 0.** *(= SC09, amended — the correctness item)*
Files: `src/exec/step_vm.cpp`, `docs/spec/waystone-concpets.md` §2/§9/§11, `tests/waystone_replay_test.cpp`.
Replay consults the instance's trail for the step's `step_id` and applies §2 per entry, falling through for that step alone on any miss. **Rule 0 lands with it and is mandatory:** a `Probe` entry is trusted only if its `pk` equals the key re-derived from the *current* producing row. Without it every §2 rule still passes on a stale trail — they validate against storage, never against the query — and the chain emits the wrong row. One implementation, shared by every replay path.
`Exists` replay is positive-only: a validated witness proves non-emptiness; a missing or invalid one means the probe runs. `NotExists` never reads a trail.
Spec edits ride here: §2 gains the negation rule and rule 0, and §11 gains the stale-probe-key test plus the note that the epoch rule is a permanently-passing stub. (§9's recording policy was already moved to `[CONFIRMED] n = 2` by V00 — a confirmed decision left listed as `[OPEN]` in a second document is the drift this merge exists to end.)
Tests: update the producing row's key column between record and replay — replay must return the *new* row; a `NotExists` step provably never reads a trail; an `Exists` step never concludes absence.
Needs: V21, waystone P11.

**V23 — Corpus and regression closure.** *(= SC10)*
Files: `tests/testdata/parser_corpus.txt`, `tests/waystone_contract_test.cpp`.
Extend the golden corpus to the full language; add the advisory-contract family — Waystone off, trails dropped mid-run, recording disabled, trail corrupted — across nested-chain fixtures, and make it regression-mandatory. Record here that blueprint-parser acceptance (phase V-6) means emitting **identical `StepChain`s** over this corpus.
Needs: V22.

*Gate:* results byte-identical across the advisory family; the per-kind replay table enforced by instrumentation.

---

## Phase V-6 — the blueprint parser

Goal: replace the implementation under the compile contract. Acceptance is V23's corpus producing identical `StepChain`s — the language does not change in this phase, only what produces it.

**V24 — Allocation-counting harness.** *(= PR03)* Test-only; never linked into `kds`. Needs: nothing.
**V25 — Zero-copy tokens.** *(= PR02)* `Token::text` becomes a view; the copy boundary is documented and singular. Needs: V24.
**V26 — Flat arena AST.** *(= PR04, amended)* Per-kind flat arrays, `NodeId` indices, per-session arena reset with capacity retained; query blocks; **sibling-linked conjuncts**, because a sub-chain's conjuncts interrupt the outer's contiguous run and no other list can be interrupted. Needs: V25.
**V27 — Parser rewrite onto the arena, and the lifetime contract.** *(= PR05, PR06)* Recursive descent unchanged in structure; debug builds catch an escaped view via an arena generation counter. Needs: V26.
**V28 — Slot table with three kinds.** *(= PR07, amended)* `kLiteral`/`kBindParam`/`kOuterRef`; the last is never written and never enters `arg_hash`. Cap behind one named `constexpr` — **collides with the `[OPEN]` slot-table cap, which must also decide whether `kOuterRef` counts. Surface, do not decide.** Needs: V27.
**V29 — `pattern_id` during the parse.** *(= PR08, amended)* Fold the separate pass in; **no version bump** — V04 already proved the language addition additive, and folding the pass must not change a single hash, which V01's `pattern_id` column pins. Needs: V28.
**V30 — pk range at PARSE and BIND.** *(= PR10)* Judged on raw digit text; nothing downstream re-checks. Needs: V28.
**V31 — Binding at PARSE, feeding the same compiler.** *(= PR19, PR20)* Names die at PARSE; the catalog version stamp invalidates cached statements exactly; the compiler's resolution step becomes a no-op consumer of bound nodes. Needs: V29.
**V32 — Named statements and KWP wiring.** *(= PR22, PR23)* The snapshot is a bounded index-space copy including the block array and slot table; `C_PARSE` returns `S_PARSE_OK{pattern_id}`; `C_BIND` computes `arg_hash` over `kLiteral`+`kBindParam` only. Against a fixture driver if `docs/inflight/in-progress/protocol-wp.md` P08 has not landed. Needs: V31.

---

## Disposition of the superseded workplans

**`step-chains-workplan.md` (SC01-SC10)** — absorbed whole, re-sequenced behind the three prerequisites of phase V-1:

| SC | → | Note |
|---|---|---|
| SC01 | V00 | plus the two extra banners this merge requires |
| SC02 | V04 | + `.` token and byte offsets |
| SC03 | V05 | + `AS` aliases and the repeated-relation rule |
| SC04 | V07 | unchanged |
| SC05 | V14 | + explicit `ColumnRef` resolution and the `PkEqualityTarget` warning |
| SC06 | V17 | + the I15 nested-access rules |
| SC07 | V18 | + the tri-state collapse and `CardinalityViolation` |
| SC08 | V21 | unchanged |
| SC09 | V22 | **+ rule 0**, without which replay is a wrong-answer generator |
| SC10 | V23 | unchanged |

**`docs/parser-workplan.md` (PR01-PR24)** — nothing died; it moved:

| | Tasks |
|---|---|
| **Absorbed** | PR01→V01, PR03→V24, PR02→V25, PR04→V26, PR05/PR06→V27, PR07→V28, PR08→V29, PR10→V30, PR11→V11, PR12→V12, PR13→V10, PR14→V08, PR15→V09, PR18→V13, PR19/PR20→V31, PR22/PR23→V32 |
| **Resequenced** | The whole Phase 1-2 foundation moves **behind** the language (phase V-6), per J4. PR19's binding moves with it, and its interim home is the step compiler (V14) |
| **Superseded in substance** | PR09 (class per statement stays, per J3 — no per-block classing); PR16's "no grammar path admits a nested SELECT" (reversed by I9); PR17→V06; PR21→V11; PR24→V17/V20 |

## Collisions with `[OPEN]` decisions — surface, never decide

| `[OPEN]` item | Collision | Blocks |
|---|---|---|
| Aggregates (I14) | J2 requires a subquery containing one to answer `Unsupported`; scalar subqueries are where `COUNT(*)` will be attempted | V07 |
| Depth cap default (4) | `[PROPOSED]` in the spec; V07 enforces whatever it is | V07 |
| Slot-table cap | Slots gain a third kind; does `kOuterRef` count? | V28 |
| `kUnclassified` in production | Chain statements are its likeliest inhabitants if compilation ever fails to classify | V14 |
| Class-list ratification | J3 settles the *shape* (absorbed into `kJoinSelect`); the list itself is still unratified | V14 |
| Buffer-pool frame reclamation | Nested access is safe today *because nothing evicts*; eviction arms it | R1 and V17's debug guard exist to survive it |
| Per-page epoch storage | Waystone §2's epoch rule has no implementation anywhere | V22 documents the stub |
| Waystone retention/eviction | Correlated `Probe` fan-out can spill a trail past one page | V21, and spec §8's open item |
| Subqueries in `INSERT`/`UPDATE` value position | `[OPEN: revisit]` | nothing — `Unsupported` until decided |
| Transactions unbuilt | A chain reads N relations with no snapshot | note in the spec; no work here |

## Out of scope — do not build in any phase

- **Aggregates.** `[OPEN]`, untouched by every task above.
- **Table-position nesting** — derived tables and CTEs. Refused with a position, by design (J2/I9).
- **Decorrelation rewrites.** Forbidden by I12, not merely unimplemented, and V20 tests for their absence.
- **A pull-based row cursor.** The storage layer offers only a push contract with no resumable position; a cursor is what portal suspension will need, and portal suspension does not exist. The step VM's outward contract is a row sink, so its internals can be replaced later without touching a caller.
- **A plan tree or any plan search.** The class tag plus the compiled chain *is* the plan (I2/I12).
