# KDS Parser — Workplan

Work instructions, companion to `docs/parser.md` (the blueprint). The blueprint's items are built as stated; its `[PROPOSED]` class list (I2) is built as the default; its `[OPEN]` item (I14, aggregates) is **not** touched by any task here.

Baseline being replaced: a recursive-descent parser with a `std::string`/`std::vector` AST, a copying lexer, no class tag, and no catalog binding. The grammar today is `CREATE TABLE` / `INSERT` / `SELECT *` / `UPDATE` with AND-only `col op val` filters. Fingerprinting already exists (`include/kds/parser/fingerprint.hpp`) as a separate pass over the same lexer; the work below folds it into the single pass rather than rebuilding it.

---

## Phase 1 — Foundation (I3, I4)

Goal: replace the representation with zero-alloc/zero-copy machinery **without changing the accepted language by one byte**. This phase is a refactor with a behavioral no-op guarantee, which is what makes it safe to do first — every later phase stamps fields onto the structures built here.

**PR01 — Golden-corpus lock-in (do this before touching code).**
Files: `tests/parser_golden_test.cpp`, `tests/testdata/parser_corpus.txt`.
Capture the current accepted/rejected behavior of every production in `ast.hpp`'s grammar comment as a text corpus (statement → serialized parse tree or exact error). This corpus is the regression oracle for PR02–PR05 and stays green through Phase 1 unchanged.
Done when: corpus covers every production and every error path in `parser.cpp`; test fails if any accepted statement changes shape.

**PR02 — Zero-copy token (I4).**
Files: `include/kds/parser/token.hpp`, `src/parser/lexer.cpp`, `tests/lexer_test.cpp`.
`Token::text` becomes `std::string_view` into the source; add `byte_offset`/`length` for exact `S_ERROR.position`. Integer literals keep `int_val` **and** the raw digit view (the existing `raw_int_text` rationale — signed `int64_t` cannot carry the full unsigned pk range).
Done when: lexing performs zero allocations (instrumented, see PR03); every token reports the byte offset of its first character; `kError` reports the offending byte.

**PR03 — Allocation instrumentation harness.**
Files: `tests/support/alloc_counter.hpp` (+ wiring in `tests/CMakeLists.txt`).
A scoped global-`operator new` counter so PR02/PR05 can assert "zero allocations in this region". Test-only; never linked into `kds`.
Done when: a scope that allocates fails the assertion and a scope that does not passes; usable from any test.
Needs: nothing. May run parallel with PR02.

**PR04 — Flat arena AST (I3).**
Files: `include/kds/parser/arena.hpp`, `include/kds/parser/ast.hpp`, `src/parser/ast.cpp`, `tests/ast_test.cpp`.
Node storage becomes per-kind flat arrays in a per-session `ParseArena` (reset per statement, capacity retained across resets — that retention is what makes steady state zero-alloc). Node references are typed indices (`NodeId`, `u32`), never pointers. Identifiers are `string_view` into the source; the copy boundary is explicit and single (`ArenaString` for anything outliving the parse). The `Statement` variant is replaced by a root record `{StatementKind, NodeId}`; `std::holds_alternative` call sites move to a kind switch.
Done when: no per-node heap allocation exists; an arena snapshot can be copied by index-space `memcpy` with no pointer fixups; `command_dispatcher.cpp` compiles against the kind switch with its tests green.
Needs: PR01–PR02.

**PR05 — Parser rewrite onto the arena.**
Files: `include/kds/parser/parser.hpp`, `src/parser/parser.cpp`, `tests/parser_test.cpp`.
Recursive descent unchanged in structure (still no backtracking, still `Status`-first, `rules.md` #1); productions emit arena nodes instead of returning owning structs. `Parser` takes an arena reference; `Parse(sql)` convenience keeps a private arena for callers that do not have one.
Done when: PR01's corpus passes byte-identically; a warm-session parse allocates zero (PR03 assertion); error messages keep their text and gain exact positions.
Needs: PR03–PR04.

**PR06 — Lifetime contract at the seam.**
Files: `include/kds/parser/ast.hpp` header comment, `docs/rules.md` cross-reference.
Document: views are valid for the statement's parse only; catalog names and cached statement text are copied at the boundary; debug builds catch an escaped view (arena generation counter checked on dereference in `KDS_DEBUG`).
Done when: the rule is stated once, and a test proving a stale `NodeId` traps in debug builds exists.
Needs: PR04–PR05.

**Phase 1 gate:** corpus green, zero-alloc/zero-copy assertions green, dispatcher untouched in behavior, `bash test.sh` green.

---

## Phase 2 — Identity (I1, I2, I6-PARSE)

Goal: the AST is born normalized and classified. This is where the engine differentiators start — Waystone gets its `pattern_id` for free, and the executor gets a dispatch tag instead of a shape analysis.

**PR07 — Literal table + parameter slots (I1).**
Files: `include/kds/parser/ast.hpp`, `src/parser/parser.cpp`, `tests/parser_literal_test.cpp`.
The lexer/parser never places a literal value in a node; each literal is appended to a statement-local literal table and the node holds a slot index. `?` bind markers occupy the same slot space, so inline-literal and bound-parameter forms produce identical node streams.
Done when: `WHERE id = 42` and `WHERE id = ?` produce identical node/slot streams differing only in the literal table's contents; the literal table has a size cap constant with its derivation comment (the cap value itself is listed open in the blueprint §4 — pick a constant behind a named `constexpr`, do not spec it as final).
Needs: Phase 1.

**PR08 — `pattern_id` during the parse (I1).**
Files: `include/kds/parser/fingerprint.hpp`, `src/parser/fingerprint.cpp`, `tests/fingerprint_test.cpp`.
Hash the token/shape stream incrementally *inside* the parse — no separate normalization pass, no post-walk. Fixed seed, no address- or pointer-derived input. `arg_hash` is a separate function over the unified (literal table + bound params) value stream, called at BIND (wired in Phase 4).
Done when: literal/param convergence holds; different shapes differ; hashes are stable across runs and processes; parsing with fingerprinting on costs no extra tree walk.
Note: this supersedes Waystone workplan **T11**, which specified a post-hoc normalization pass over the old `Statement` variant. Mark T11 as delivered-by-PR08 in `docs/waystone-workplan.md`.
Needs: PR07.

**PR09 — Statement class tags (I2).**
Files: `include/kds/parser/ast.hpp`, `src/parser/classify.cpp`, `tests/parser_class_test.cpp`.
Stamp exactly one `StatementClass` on the AST root at parse time: `kPointSelect`, `kRangeSelect`, `kJoinSelect`, `kPointUpdate`, `kPointDelete`, `kInsert`, `kCreateTable`, `kSessionSet`, `kAdminShow`, `kUnclassified`. Classes whose grammar does not exist yet (join, session, admin) are declared now and become reachable in Phase 3 — declaring the full enum here keeps the executor switch stable across phases.
Done when: every production maps to exactly one class (exhaustive table test); `kUnclassified` is reachable only by design and is counted by a metric; property test — every successful parse yields exactly one class.
Needs: PR07.

**PR10 — pk literal range enforcement, PARSE half (I6).**
Files: `src/parser/parser.cpp`, `tests/parser_pk_range_test.cpp`.
Any literal in a pk position is checked `< 2^40` and rejected with `InvalidArgument` + exact byte position. Enforced on the raw digit text, not the `int64_t`, so the full unsigned range is judged correctly. Nothing downstream re-checks.
Done when: `2^40 - 1` accepted, `2^40` and `u64` max rejected at PARSE with positions; fuzzed boundary values pass.
Note: "pk position" is currently syntactic (first column / `id`); it becomes catalog-exact in PR20. Both must agree — PR20 ships a test proving no statement changes verdict between the two.
Needs: PR07.

**Phase 2 gate:** fingerprint stability + convergence tests green; class mapping exhaustive; pk range fuzz green.

---

## Phase 3 — Grammar surface (I7, I8, I9, I10, I11, I13)

Goal: the v1 language. Every task here is additive to Phase 1–2 machinery — new productions emit arena nodes, feed the running hash, and land in an existing class.

**PR11 — `CREATE TABLE ... WITH (k = v, ...)` (I7).**
Files: `src/parser/parser.cpp`, `include/kds/parser/table_options.hpp`, `tests/parser_ddl_test.cpp`.
Option **table**, not grammar productions; the v1 entry is `PHYSICAL_OPTIMIZER = ON|OFF`. Unknown option → parse error naming the option and its position. Adding an option must require no grammar edit.
Done when: adding a third option is a one-line table entry (proven by a test-only option); both v1 options reach the AST as flags (catalog wiring is PR21).

**PR12 — Session/admin statements (I8).**
Files: `src/parser/parser.cpp`, `tests/parser_admin_test.cpp`.
`SET DURABILITY {STRICT|GROUP|RELAXED}` → `kSessionSet`; `SHOW META`, `SHOW TABLES` → `kAdminShow`; `SHOW PROFILE` registered only under `KDS_PROFILING`.
Done when: `kSessionSet` statements are excluded from fingerprinting (no `pattern_id` emitted, asserted); `SHOW PROFILE` is absent from a release-build grammar (test compiled both ways).

**PR13 — DELETE.**
Files: `src/parser/parser.cpp`, `tests/parser_test.cpp`.
The blueprint names `kPointDelete` in I2 but §2 never states a DELETE production — the class list requires it. `DELETE FROM <rel> WHERE <conjuncts>` with the same predicate surface as UPDATE.
Done when: pk-equality DELETE classifies `kPointDelete`; unrestricted DELETE (no WHERE) is a decision point — ship it rejected as `Unsupported` in v1 and flag for ratification rather than silently allowing a full-table delete.

**PR14 — Predicate surface: `IN`, `BETWEEN` (I10).**
Files: `src/parser/parser.cpp`, `tests/parser_predicate_test.cpp`.
Predicate AST is a flat conjunct array (no expression tree). `IN (list)` and `BETWEEN a AND b` join `col op {param}`. `OR`/`NOT`/parenthesized expressions are rejected as `Unsupported` with position — truthful, not a syntax error.
Done when: no supported predicate form ever downgrades a statement to `kUnclassified`; `IN` list elements occupy literal-table slots like any literal (so `IN (1,2)` and `IN (?,?)` converge).
Needs: PR07, PR09.

**PR15 — `ORDER BY` pk + `LIMIT`/`OFFSET` (I11).**
Files: `src/parser/parser.cpp`, `tests/parser_pagination_test.cpp`.
`ORDER BY <pk> [ASC|DESC] LIMIT n [OFFSET m]`. Non-pk ORDER BY → `Unsupported` + position.
Done when: pagination round-trips through `kRangeSelect`; limit/offset values are literal-table slots (pagination varies by value, not by shape — `LIMIT 10` and `LIMIT 20` must share a `pattern_id`).
Needs: PR14.

**PR16 — Inner equi-join chains (I9).**
Files: `src/parser/parser.cpp`, `tests/parser_join_test.cpp`.
2+ relations, `JOIN ... ON a.col = b.col`, chained, flat. `LEFT`/`RIGHT`/`FULL`/`OUTER` lexed and reserved, rejected `Unsupported` with position. No grammar path admits a nested SELECT, CTE, or derived table — the recursive-descent entry for a relation reference must not reach the statement production.
Done when: chained equi-joins classify `kJoinSelect`; qualified column references (`a.col`) resolve syntactically; a fuzz corpus of nested-SELECT attempts all fail as syntax errors, never as accepted parses.
Needs: PR14.

**PR17 — Column projection.**
Files: `src/parser/parser.cpp`.
`SELECT *` only is a Phase-1 inheritance; joins (PR16) make an explicit column list unavoidable (`SELECT a.x, b.y`). Ship the select list as an arena node array.
Done when: `*`, explicit lists, and qualified names all parse; projection shape does not affect statement class.
Needs: PR16.

**PR18 — Grammar hygiene + fuzz (I13).**
Files: `tests/parser_fuzz_test.cpp`, `docs/parser.md` grammar section.
Byte-level and token-level fuzzing: the parser never crashes, never allocates unboundedly, always returns `Status`. Grammar spec cites only KDS documents; no "for compatibility" production exists (grep-enforced).
Done when: fuzz runs clean at the agreed iteration count in CI; golden parse trees exist for every production.
Needs: PR11–PR17.

**Phase 3 gate:** every I2 class reachable from real syntax; every reserved-but-unsupported form returns `Unsupported` with an exact position (never a bare syntax error); fuzz clean.

---

## Phase 4 — Binding & integration (I5, I6-BIND, I12)

Goal: the parser becomes the front door — names die at PARSE, EXECUTE sees only oids and a class tag. This phase is gated on subsystems outside `parser/`; each task names its seam.

**PR19 — Catalog binding at parse (I5).**
Files: `src/parser/bind.cpp`, `include/kds/parser/bind.hpp`, `tests/parser_bind_test.cpp`.
Resolve relation → oid and column → id/type during the parse; the AST carries oids and the executor never sees a name. Type names resolve here too (retiring `ColumnDef::type_name`'s "unresolved" note) once the type registry exists — until then resolve against `catalog::` well-known types and leave the unresolved path behind one function.
Seam: `kds::catalog::Catalog` read path.
Done when: a bound AST contains no identifier strings; EXECUTE performs zero catalog name lookups (asserted by a lookup counter).
Needs: Phase 3.

**PR20 — Catalog version stamp + invalidation (I5).**
Files: catalog version counter, `src/parser/statement_cache.cpp`, `tests/parser_invalidation_test.cpp`.
Every parsed statement carries the catalog version at bind time; DDL bumps it; a stale stamp forces transparent re-parse, named statements included. Also lands the catalog-exact pk position check promised by PR10.
Done when: two-session test — session A caches a statement, session B runs `CREATE TABLE`/`ALTER`-class DDL, session A's next EXECUTE transparently re-parses and succeeds; unaffected statements are not invalidated.
Needs: PR19.

**PR21 — DDL options → catalog flags (I7 completion).**
Files: catalog relation record, `tests/catalog_test.cpp`.
The `PHYSICAL_OPTIMIZER` option round-trips into a catalog flag on the relation record. There is no `WAYSTONE` option: Waystone is keyed on a pattern, not a relation, so nothing per-relation exists for it to set.
Needs: PR11, PR19.

**PR22 — Named statements as arena snapshots (I3 completion).**
Files: `src/parser/statement_cache.cpp`, `tests/statement_cache_test.cpp`.
A cached named statement is a bounded index-space copy of the arena, not a tree walk. Session-scoped; survives until `C_CLOSE` or disconnect; the unnamed statement (`""`) is overwritten by the next PARSE (`docs/protocol.md` §5).
Done when: caching a statement performs one bounded copy with no pointer fixups; re-execution from cache allocates zero.
Needs: PR04, PR20.

**PR23 — KWP `C_PARSE` / `S_PARSE_OK` / BIND wiring (I1, I6-BIND).**
Files: KWP session layer, `tests/kwp_parse_test.cpp`.
`C_PARSE` runs the parser once and returns `S_PARSE_OK{pattern_id}`; `C_BIND` computes `arg_hash` over the unified value stream and range-checks pk **parameters** (`< 2^40`, `InvalidArgument`, exact parameter index). The parser runs at `C_PARSE` only, never per execution.
Seam: the KWP session state machine (`docs/protocol-wp.md` P08) does not exist — only `include/kds/wire/kwp.hpp` + `src/wire/frame_codec.cpp` do. If P08 has not landed, PR23 ships the parse/bind entry points against a fixture driver and the frame wiring moves into P08.
Needs: PR08, PR10, PR22.

**PR24 — Executor switch dispatch + written-order joins (I2, I12).**
Files: `src/server/command_dispatcher.cpp` (executor successor), `docs/client-manual.md`, `tests/exec_dispatch_test.cpp`.
The executor dispatches on the class tag with a `switch` and contains no shape re-analysis. Join execution order is textual order — nested-loop via pk index into each subsequent relation, never reordered. State the contract in the client manual.
Done when: grep finds no shape analysis in the executor; a join test proves execution order matches text order for both orderings of the same query.
Needs: PR19, PR23.

**Phase 4 gate:** zero name lookups at EXECUTE; invalidation exact across sessions; pk range enforced at PARSE and BIND with nothing downstream re-checking; join-order contract documented and tested.

---

## Out of scope (do not build in any phase)

- **I14 aggregates** (`COUNT/SUM`, `GROUP BY`) — `[OPEN]` in the blueprint. Neither exclusion nor keyword reservation is decided; no task above touches it.
- Per-statement literal-table size cap as a *specified* value, and whether `kUnclassified` is permitted in production builds — both listed open in blueprint §4. PR07/PR09 keep each behind a named constant / metric so either resolution stays viable.
- The I2 class list is `[PROPOSED]`; PR09 builds it as stated. Amending it later is a one-enum change plus the executor switch, by design.

## Blocked references

There is no physical-optimizer specification. The `PHYSICAL_OPTIMIZER` DDL option (PR11) is parsed and stored as a flag regardless; nothing consumes it yet.
