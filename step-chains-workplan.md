# KDS Step Chains — Workplan

Work instructions, companion to `docs/step-chain.md`. Task ids `SC##` (no collision with P/PR series). Execution rules as everywhere: numeric order unless "needs" says otherwise; tests ship in the same change; `bash test.sh` green is part of "done"; touching a spec `[OPEN]` means stop and flag. The advisory-contract test family is regression-mandatory from SC08 onward.

Vehicle note (J4): SC02–SC04 extend the **current** recursive-descent parser. The blueprint parser (`docs/parser-workplan.md`) replaces the implementation later but must emit identical `StepChain`s — SC05's compile contract and SC10's corpus are what make that a checkable statement, not a hope.

---

**SC01 — Document amendments (gate).**
Apply `step-chain.md` §8 items 1–5: parser.md I9 amendment (dated), waystone-concpets §2/§7/§9 updates (negation rule; n = 2 confirmed), error registry `CardinalityViolation`, client-manual subquery section, parser-workplan bolt-on note.
Done when: grep finds no "out of scope, not deferred" claim for predicate subqueries; specs cross-reference cleanly.

**SC02 — Lexer: new tokens.**
`JOIN`, `ON`, `IN`, `EXISTS`, `NOT`, `BETWEEN` keywords; sub-select `(SELECT` handling. Tokens are shape for the fingerprint (no version bump — additive); pin with a test that every pre-existing corpus statement's `pattern_id` is unchanged.
Tests: lexing goldens; fingerprint invariance on the old corpus.
Needs: SC01.

**SC03 — Grammar: joins (bolt-on).**
`FROM a JOIN b ON a.col = b.col [JOIN c ON …]` in the current parser + AST nodes (join list with per-join equi-condition). Outer-join keywords lex and answer `Unsupported` with position.
Tests: parse goldens; written-order preservation in the AST; outer keywords' truthful errors.
Needs: SC02.

**SC04 — Grammar: predicate-position subqueries.**
Scalar comparison (`=`, `<`, …) against `(SELECT …)`; `IN`/`NOT IN (SELECT …)`; `EXISTS`/`NOT EXISTS (SELECT …)`; recursion to depth 4 with a counted guard; J2 forms (FROM-position, aggregates inside, over-depth) → `Unsupported` with exact position.
Tests: parse goldens per form; depth-cap boundary (4 parses, 5 errors); every `Unsupported` surface hit.
Needs: SC03.

**SC05 — Step compiler (the contract).**
`Compile(AST) → StepChain` in `src/exec/`: global step numbering in compile order; access-kind assignment per `step-chain.md` §1; uncorrelated sub-chains hoisted as prefixes; correlated ones as nested steps with probe-key bindings; class tagging (`kJoinSelect` absorption). Pure function of the AST — same statement, same chain, always.
Tests: chain goldens per statement form; numbering stability; purity (two compiles bit-identical).
Needs: SC03–SC04.

**SC06 — Step VM: linear chains.**
Executor loop over `Lookup`/`Probe`/`Range`/`Scan` steps against btree/heap relations; cursor frames preallocated (no per-row allocation, instrumented); wired into the command path for `kJoinSelect`.
Tests: oracle equivalence vs a naive evaluator on join fixtures; allocation instrumentation.
Needs: SC05.

**SC07 — Step VM: nested chains & negation.**
Per-outer-row sub-chain execution; `Exists`/`NotExists` semantics; scalar cardinality check (`CardinalityViolation` at the second row); `NOT IN` three-valued NULL semantics.
Tests: oracle equivalence per subquery form; NULL-in-`NOT IN` fixture; cardinality violation raised and mapped through the error registry.
Needs: SC06.

**SC08 — Trail recorder (n = 2).**
Core-local bounded instance-sighting table (`(pattern_id, arg_hash)` → count); second execution records: per-step entries for `Lookup`/`Probe`, witness-only for `Exists`, nothing for search/negation steps; writes go through the existing waystone page codec + directory (waystone-workplan's directory task is a prerequisite — coordinate ids).
Tests: first-run-silent / second-run-records (instrumented); per-kind recording table enforced; sighting-table eviction restarts the count harmlessly.
Needs: SC06–SC07 + waystone directory task.

**SC09 — Trail replay.**
Before each replay-eligible step, consult the trail for its `step_id` and apply the four-rule replay contract per entry; miss ⇒ authoritative path for that step alone; search-class steps receive the trail only as a prefetch batch; `NotExists` provably never consults it.
Tests: replay-vs-authoritative byte equality (oracle); per-entry fallback on injected staleness (epoch bump, moved rows); instrumented negative rules (§6-3 of the spec); performance smoke on the 3-relation join fixture (the concept doc's §7 bar).
Needs: SC08.

**SC10 — Corpus & regression closure.**
Extend the parser golden corpus with the full new language; add the advisory-family run (Waystone off / trails dropped mid-run / recording disabled ⇒ byte-identical results) across nested-chain fixtures to the mandatory set; record in `parser-workplan.md` that blueprint-parser acceptance = identical `StepChain`s over this corpus.
Needs: SC02–SC09.
