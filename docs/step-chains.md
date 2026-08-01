# KDS Step Chains — Joins, Subqueries & the Waystone Trail (Specification)

**Status:** **Official specification**, decisions confirmed 2026-08-01 (J1–J5, §0). Amends `docs/parser.md` I9 (§8-1) and generalizes the execution concept behind `kJoinSelect`. Companion tasks: `docs/step-chain-workplan.md`. Markers: `[CONFIRMED]`, `[PROPOSED]`, `[OPEN]`. Consistent with `docs/waystone-concpets.md` (trail model, replay contract), `docs/parser.md`, `docs/rules.md`, `docs/txn.md`.

## 0. Decision Record `[CONFIRMED 2026-08-01]`

| # | Decision | Choice |
|---|---|---|
| J1 | Subquery scope | **Wide**: predicate-position subqueries — scalar (uncorrelated *and* correlated), `IN`/`NOT IN (SELECT …)`, `EXISTS`/`NOT EXISTS` — with nesting to a fixed depth cap. Not restricted to flattenable forms |
| J2 | Inexecutable forms | **`Unsupported`** with exact position — never a slow generic path. Table-position nesting (derived tables, CTEs), subqueries containing aggregates (I14 still open), and depth beyond the cap are truthful errors |
| J3 | Classification | Absorbed into **`kJoinSelect`**; the documented concept generalizes from "join chain" to **step chain**. No new enum value |
| J4 | Vehicle | **Bolt-on to the current recursive-descent parser** — do not wait for the blueprint parser. The AST→StepChain compile contract is the seam that survives the parser replacement |
| J5 | Trail recording policy | **n = 2**: an instance's trail is recorded on its **second** execution (first execution only counts) |

## 1. The Step Chain `[CONFIRMED]`

Every SELECT-class statement compiles to a **step chain**: an ordered list of steps, each reading one relation with one access kind. Written order is execution order (`parser.md` I12 — the query is the plan — now generalized: *the statement is the chain*).

Step access kinds:

| Kind | Authoritative work | Trail-replayable? |
|---|---|---|
| `Lookup` | pk-equality descent (constant or bound param) | **yes** — completeness follows from pk uniqueness |
| `Probe` | pk-equality descent keyed by a value produced by an earlier step / outer row | **yes** — same argument, per producing row |
| `Range` | pk range via leaf chain | no — search; prefetch only |
| `Scan` | heap chain / full scan with predicate | no — search; prefetch only |
| `Exists` | semi-join probe: stop at first qualifying row | **positive result only** (§4) |
| `NotExists` | anti-join: prove absence | **never** — absence has no witness |

A join contributes one `Lookup`/`Probe`/`Scan` step per relation in written order. This table *is* the trail trust model of `waystone-concpets.md` §2 ("a waystone may replace a lookup, never a search"), extended with the negation rule: **negation steps are search-class by definition.**

## 2. Subqueries as Steps `[CONFIRMED shape; details PROPOSED]`

- **Uncorrelated subquery** → a **prefix sub-chain**: hoisted, executed exactly once before the outer chain, its result bound as a value (scalar) or a probe set (`IN`).
- **Correlated subquery** → a **nested sub-chain step**: executed once per outer row, its correlation columns arriving as `Probe` keys. A correlated pk-equality subquery therefore costs exactly what a join step costs — and records the same trail shape.
- **Scalar cardinality:** a scalar subquery yielding more than one row is a runtime error, `CardinalityViolation` (new error-registry code, retryable = 0). Parse time cannot prove cardinality in general; the check is per execution.
- **`IN` (positive)** compiles to `Exists` per outer row over the sub-chain (or a hoisted probe set when uncorrelated). **`NOT IN` / `NOT EXISTS`** compile to `NotExists`. `NOT IN` keeps standard SQL three-valued semantics: any NULL in the subquery result makes the predicate never-true — implemented, tested, and called out in the client manual as the standard foot-gun it is.
- **Depth cap:** sub-chains nest to depth **4** `[PROPOSED default]`; deeper is `Unsupported`.
- **Out of scope (J2, `Unsupported`):** subqueries in FROM (derived tables), CTEs, subqueries containing `GROUP BY`/aggregates (blocked on I14), subqueries in `INSERT`/`UPDATE` value position `[OPEN: revisit]`.

Step numbering is global in compile order (outer chain and sub-chains share one counter), so `step_id` in a trail entry is unambiguous without parent linkage; the chain layout is a pure function of the AST, hence of `pattern_id` — which is what makes recorded trails replayable across executions of the same instance.

## 3. Classification & Fingerprints

- All step-chain statements classify as `kJoinSelect` (J3); single-relation point/range forms keep their existing classes. Documentation should read `kJoinSelect` as "step-chain select".
- Fingerprints need **no changes and no version bump**: the token-stream hash absorbs the new keywords/parentheses as shape; every previously-accepted statement hashes identically (additive language change). New lexer tokens (`JOIN`, `ON`, `IN`, `EXISTS`, `BETWEEN`, sub-select parens) are shape, per the P01 rules.

## 4. Trail Integration `[CONFIRMED policy; mechanics PROPOSED]`

- **Recording (J5, n = 2):** the first execution of an instance `(pattern_id, arg_hash)` only counts; the second records the trail. Instance sighting is tracked in a bounded, core-local in-memory table; eviction from that table merely restarts the count (a performance event). Pattern-level `use_count` in `sys.patterns` continues independently for retention.
- **Per-step recording:** `Lookup`/`Probe` steps append entries (execution order, `step_id`, per-entry `rel_oid` — the existing format, unchanged by this spec). `Exists` steps record the witnessing row only. `Range`/`Scan`/`NotExists` steps record nothing.
- **Replay:** before running a replay-eligible step, the executor consults the instance's trail for entries with that `step_id` and applies the four-rule replay contract (`waystone-concpets.md` §2) per entry; any miss falls through to the authoritative path *for that step alone*. `Exists` replay may confirm existence via a validated witness but a missing/invalid witness means the probe runs — a trail can never conclude absence. Search-class steps use the trail only as a prefetch batch.

## 5. Executor `[PROPOSED]`

A small step VM in `src/exec/`: compile (AST → `StepChain`, done at PARSE alongside class tagging), then iterate — linear loop over steps, nested loop for sub-chain steps, all run-to-completion on the owning core. Cursor state lives in the chain frame (no allocation per row; bounded by depth cap × per-step state). The VM is the compile contract J4 preserves: when the blueprint parser lands, it emits the same `StepChain` and nothing downstream changes.

## 6. Testing Requirements

1. **Oracle equivalence:** every step-chain statement checked against a naive reference evaluator over small fixtures — joins, each subquery form, NULL-in-`NOT IN`, cardinality violations.
2. **Advisory family:** results byte-identical with Waystone off / trails dropped mid-run / recording disabled — now exercised specifically across nested chains.
3. **Replay rules:** per-kind table of §1 enforced by instrumentation — a `NotExists` step provably never reads a trail; an `Exists` step never concludes absence from one.
4. **n = 2:** first execution records nothing (instrumented); second records; eviction of the sighting entry restarts the count without correctness change.
5. **`Unsupported` surfaces:** every J2 form errors with exact position; nothing falls through to a generic path.
6. **Depth & numbering:** chains at the depth cap; global `step_id` stability across executions of one instance.

## 7. Open Decisions — do not assume

- Depth cap default (4 `[PROPOSED]`); instance-sighting table size; `IN`-list vs `IN (SELECT)` hoisted-set size caps.
- Subqueries in `INSERT`/`UPDATE` value position.
- Trail entry semantics for multi-row `Probe` fan-out beyond the page cap (spills to `next_page_id` chain — cap per instance inherited from waystone-concpets §9 retention `[OPEN]`).

## 8. Required Amendments (gate)

1. **`docs/parser.md` I9 — amended 2026-08-01:** replace "nested structures are out of scope, not deferred" with: predicate-position subqueries per §2 are **in scope**; table-position nesting (derived tables, CTEs) remains out and answers `Unsupported`. Record the reversal date; I2's class list note gains "kJoinSelect = step-chain select".
2. **`docs/waystone-concpets.md`:** §2 gains the negation rule ("negation steps are search-class by definition"); §7 gains the subquery cases; recording policy n = 2 moves from `[OPEN]` (§9) to `[CONFIRMED]`.
3. **Error registry:** add `CardinalityViolation` (retryable 0).
4. **Client manual:** subquery forms, depth cap, `NOT IN` NULL semantics, `Unsupported` surfaces.
5. **`docs/parser-workplan.md`:** note the bolt-on (J4) and the golden-corpus extension it requires; the blueprint parser's acceptance now includes emitting identical `StepChain`s for the extended language.
