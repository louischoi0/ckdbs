# KDS Parser — Blueprint & Development Instructions

**Status:** **Official blueprint**, directions confirmed 2026-07-29. This document is written as *instructions*: each item states what to build and when it counts as done. Markers: `[CONFIRMED]` — build as stated; `[PROPOSED]` — default within a confirmed direction, amend before implementing if needed; `[OPEN]` — do not assume. Consistent with `docs/rules.md`, `docs/protocol.md` (KWP §5), `docs/waystone-concpets.md` (fingerprint layer), `docs/physical-optimizer-blueprint.md` (bundle input — **document does not exist yet**), and the design spec (invariant 6). Task breakdown: `docs/parser-workplan.md` (4 phases, PR01–PR24).

The parser runs at KWP `C_PARSE` only — never per execution (extended model, D4). It is therefore off the per-execution hot path, but it is the **front door of every engine differentiator**: fingerprints for Waystone, statement classes for plan-free dispatch, and the DDL surface for engine features all originate here.

---

## 1. Architecture Instructions

**I1 — Parameterize literals at parse time (pattern_id for free). `[CONFIRMED]`**
The lexer never places literal values into the AST. Every literal is pushed into a statement-local literal table; the AST node holds a parameter-slot index. The AST is therefore born normalized: `pattern_id` = hash of the token/shape stream, computed during the parse itself — no separate normalization pass, ever. Queries with inline literals and queries with bind parameters converge to the same `pattern_id`; `arg_hash` is computed over the unified (literal table + bound params) value stream at BIND.
*Done when:* `PARSE` of `WHERE id = 42` and `WHERE id = ?` yield identical `pattern_id`; fingerprint cost is zero additional passes; `S_PARSE_OK{pattern_id}` (KWP §5) is fed from this path.

**I2 — Tag statement classes (switch dispatch, no plan search). `[CONFIRMED]`**
The parser classifies every statement's execution shape at parse time and stamps it on the AST root. The executor dispatches on this tag with a `switch` — no plan enumeration exists in v1. v1 class list `[PROPOSED]`: `kPointSelect` (single-relation, pk-equality), `kRangeSelect` (pk range), `kJoinSelect` (equi-join chain), `kPointUpdate`, `kPointDelete`, `kInsert`, `kCreateTable`, `kSessionSet`, `kAdminShow`, `kUnclassified` (parses but takes the slow generic path). The tag is also the trust-classification input for the hint index (unique-lookup classes are the trusted candidates).
*Done when:* every grammar production maps to exactly one class; the executor contains no shape re-analysis; `kUnclassified` exists as the safety valve and is metered.

**I3 — Per-session arena + index-based flat AST (zero alloc). `[CONFIRMED]`**
AST nodes live in flat arrays owned by a per-session parse arena (reset per statement); node references are indices, not pointers. No heap allocation per node; steady-state allocation on the parse path is zero after session warm-up. A cached named statement is the arena snapshot — storing it is a bounded copy, not a tree walk.
*Done when:* an allocation-counting test shows zero allocations parsing a warm session's statement; AST equality/serialization works by index-space copy.

**I4 — Zero-copy tokenizing via string_view over KWP frames. `[CONFIRMED]`**
Tokens reference the `C_PARSE` frame payload in place (`std::string_view`); nothing is copied during lexing. Lifetime contract: views are valid for the statement's parse only; anything that outlives the parse (names entering the catalog, cached statement text) is copied explicitly at that boundary, and the boundary is the *only* place copies happen. The lexer tracks byte offsets so `S_ERROR.position` is exact.
*Done when:* lexer performs no copies (instrumented); error positions point at the offending byte; lifetime rules documented at the seam.

**I5 — Catalog binding at parse + DDL version-stamp invalidation. `[CONFIRMED]`**
Name resolution (relation → oid, column → id/type) happens at PARSE, not EXECUTE. The AST carries oids; the executor never sees names. Every parsed statement is stamped with the catalog version at bind time; DDL bumps the version, and a stale stamp forces transparent re-parse on next use (named statements included).
*Done when:* EXECUTE performs zero catalog name lookups; a `CREATE TABLE`/`ALTER`-class change invalidates affected cached statements exactly (test with two sessions).

**I6 — Enforce the 40-bit pk literal range (invariant 6 at the front door). `[CONFIRMED]`**
Any literal or parameter destined for a pk position is range-checked: value < 2⁴⁰, rejected otherwise with `ERROR(InvalidArgument)` and the exact position. The upper-24-bits-zero rule is enforced before anything downstream can observe a violation.
*Done when:* out-of-range pk literals fail at PARSE, out-of-range pk binds fail at BIND, and no downstream component re-checks.

## 2. Grammar Surface

**I7 — KDS DDL surface: CREATE TABLE WITH options. `[CONFIRMED]`**
`CREATE TABLE ... WITH (key = value, ...)` with an extensible option table. v1 options: `WAYSTONE = ON|OFF` (catalog flag, waystone-concept §7) and `PHYSICAL_OPTIMIZER = ON|OFF` (blueprint §8 per-relation gate). Unknown options are a parse error naming the option. Future engine features add options — never new syntax.
*Done when:* both options round-trip into catalog flags; option table addition requires no grammar change.

**I8 — Session/admin statements. `[CONFIRMED]`**
`SET DURABILITY {STRICT|GROUP|RELAXED}` (wire-protocol P03), `SHOW META`, `SHOW TABLES`, `SHOW PROFILE` (registered only in `KDS_PROFILING` builds). Admin statements are ordinary statements returning ordinary result sets over KWP — one surface, one auth story.
*Done when:* SET statements are excluded from fingerprinting (non-pattern class); `SHOW PROFILE` is absent from release-build grammar.

**I9 — Join scope: inner equi-join chains; reserve outer syntax only. `[CONFIRMED]`**
v1 parses inner equi-joins over 2+ relations (`JOIN ... ON a.col = b.col`, chained). `LEFT/RIGHT/FULL OUTER` keywords are lexed and reserved but rejected with `Unsupported` (not a syntax error) so clients get a truthful error and the grammar doesn't shift when they land. Nested structures — subqueries, CTEs, derived tables — are **out of scope**, not deferred: flat multi-relation reference with filters is the language.
*Done when:* `kJoinSelect` covers chained equi-joins; outer-join keywords produce `Unsupported` with position; no grammar path admits a nested SELECT.

**I10 — Filter scope: AND-combined comparisons + IN, BETWEEN. `[CONFIRMED]`**
Predicates are conjunctions of `col op {param}` comparisons, `IN (list)`, and `BETWEEN`. `OR` and arbitrary expression trees are excluded from v1 — they blur statement classification (I2), which is worth more than expressiveness here.
*Done when:* the predicate AST is a flat conjunct array; the classifier never downgrades to `kUnclassified` because of a supported predicate form.

**I11 — ORDER BY pk + LIMIT (pagination is mandatory in v1). `[CONFIRMED]`**
`ORDER BY <pk> [ASC|DESC] LIMIT n [OFFSET m]` parses in v1 — pagination is table stakes for OLTP clients. Ordering by non-pk columns is `Unsupported` in v1 (the semi-sorted heap + B+ tree make pk order the free order).
*Done when:* pk-ordered pagination round-trips through `kRangeSelect`/`kJoinSelect` classes; non-pk ORDER BY errors truthfully.

**I13 — Dialect compatibility is a non-goal. `[CONFIRMED]`**
No PG/MySQL syntax emulation, no quoting-rule mimicry, no compatibility shims. The grammar is exactly what KDS needs (D1: custom protocol, own client libraries). Compatibility requests are product decisions, not parser patches.
*Done when:* the grammar spec cites KDS documents as its only sources; no "for compatibility" production exists.

## 3. Execution Contract

**I12 — Join order = written order ("the query is the plan"). `[CONFIRMED]`**
With no query optimizer, join execution order is the textual order: start at the first relation, nested-loop via pk index into each subsequent one. This is a *documented client contract*, not a limitation to apologize for — deterministic, predictable, appliance-appropriate. When a query optimizer eventually exists, this class remains available as the fast path.
*Done when:* the contract is stated in the client manual; executor order provably matches text order; reordering never happens silently.

## 4. Open Decisions — do not assume

**I14 — Aggregates (`COUNT/SUM`, `GROUP BY`). `[OPEN]`**
Needed eventually for the financial domain; heavy for the v1 executor. Decide: exclude from v1 grammar entirely vs reserve keywords with `Unsupported` (I9-style). Do not implement either path until decided.

Also open: exact v1 class list ratification (I2 `[PROPOSED]` list), per-statement literal-table size caps, whether `kUnclassified` is allowed in production builds or gated.

## 5. Testing Requirements

1. **Fingerprint:** literal/param convergence (I1); SET/DDL excluded; stability across runs (no address-based hashing) — extends the existing T11 tests.
2. **Classification:** exhaustive production→class mapping; classifier property tests (every parse yields exactly one class; supported predicate forms never unclassify).
3. **Zero-copy/zero-alloc:** instrumented lexer (no copies) and warm-parse (no allocations); lifetime violation tests (view escaping the statement boundary is caught in debug builds).
4. **Catalog binding:** oid-resolved ASTs; version-stamp invalidation across sessions; re-parse transparency.
5. **Invariant 6:** pk range enforcement at PARSE and BIND; fuzzed boundary values (2⁴⁰−1, 2⁴⁰, u64 max).
6. **Grammar:** golden parse trees for every production; fuzzing (byte-level and token-level) — parser never crashes, always `Status`; reserved/unsupported forms return `Unsupported` with exact positions.
7. **DDL options:** WITH round-trips to catalog flags; unknown option errors name the option.

## 6. Required Amendments

1. `include/kds/parser/ast.hpp`: flat-AST migration (I3) and class tag (I2) — coordinate with existing consumers (fingerprint T11, KWP session P08).
2. `docs/protocol.md` / client manual: document the I12 join-order contract and I9/I11 `Unsupported` surfaces.
3. `docs/waystone-concpets.md` (pattern layer) and `docs/physical-optimizer-blueprint.md` (bundle planner): note `kJoinSelect` as the pattern-group candidate tag. *(The physical-optimizer document does not exist; that half is blocked, see the workplan's "Blocked references".)*
4. `CLAUDE.md`: parser summary lines (parameterized-at-parse, statement classes, written-order joins) + I14 in the open list.
