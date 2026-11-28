# Work order EM - the statement front end's repeated work, cut only where no cut can change an answer

Status: **WRITTEN 2026-11-28, not opened.** Written on worktree
`front-end-parse-reuse-order` (branch `worktree-front-end-parse-reuse-order`)
from `72580aa04` (`v3.5.1-6-g72580aa04`) on the operator's word of
2026-11-28 (`raft-marks-2026-11-28.md` §8). No stage is started and no code
is written; EM-S0 waits for the operator's word and for the marks on §5's
items. Every `[source-read]` claim with a `path:line` was read by CLA at
`72580aa04`. A claim marked "EA's census" was read by EA's session at
`481d101aa` and is re-verified at S0.

Tags: `[operator]`, `[source-read]`, `[measured]`, `[design]`, `[assumed]`,
`[quiet-wrong]`, as in DT's order.

Related: `docs/spec/protocol.md` §5 (the kept parse, EA-R1..R6);
`docs/spec/parser-v2.md`; `docs/spec/namespace.md` NS5, NS6;
`docs/spec/catalog.md` CT2-CT7; `include/kds/catalog/catalog_cache.hpp`;
`include/kds/parser/fingerprint.hpp`; EA's order
(`workorder-ea-prepared-statement-reuse.md`), CX-S6 and CX-Q37
(`workorder-cx-managed-plans.md`); `docs/inflight/known-gaps.md:1733-1736`,
`:3045-3051`.

## 0. What EM is

A text statement's front end - its lex, its parse, its fingerprint and the
resolution of the names it writes - is done again at every execution, and in
two places it is done twice. EM cuts that repeated work **only where the cut
cannot change what any statement answers**. It works in three steps: a
census and a premise measurement (S0), a proof of concept for each
candidate S0 admits (S1), and an implementation only for a candidate whose
proof of concept passes its gate (S2 onwards).

**The admission test (EM-Q2 (a)) decides every candidate.** If anything in
the cut is wrong, the worst it may do is one of three things: refuse a
statement, give a slower plan, or abort a debug build. It may never give a
different answer. A candidate shows this by construction - EM-R2, EM-R3 and
EM-R4 are the three things it must show - and not by tests alone. A
candidate that cannot show them is dropped, not built behind a flag.

### 0.1 Excluded, and why

These came up in the same discussion and are out of EM. The first three can
give a different answer; the fourth is excluded for another reason:

- **The server auto-prepares by shape.** Literals become holes and a
  `KeptParse` is reused by `pattern_id`. Keyed by the 64-bit hash, a
  collision binds values into another statement's AST: `[quiet-wrong]`.
  Keyed whole, it still makes every literal a hole, where EA made only the
  client's `?` a hole, and a literal that is shape breaks EA-R1. `NULL` is
  one known case (`kept_parse.hpp:42-45`); whether there are others is not
  known.
- **A compiled-chain cache.** CX-Q37 (a): "a chain cached without its
  literals and bound again per statement is the one structure in this
  order that could answer wrongly" (`workorder-cx-managed-plans.md:589`).
- **Skipping the written order's first compile**: "`CompileBlock` compiles
  the written order first - its refusal is the statement's"
  (`known-gaps.md:3061-3062`). Skipping it can turn the written order's
  refusal into an answer, which is `[quiet-wrong]` by this project's own
  definition.
- **Relation names unique across all namespaces.** It gives no wrong
  answer, but it contradicts NS6 (`namespace.md:158`), and CLA's reading in
  this session gives it about one hash probe per relation bound.

## 1. The tree as read

### 1.1 The text path's front end `[source-read]`

- **`C_PARSE` lexes.** For every statement it calls
  `parser::FingerprintOf(stmt.sql)` to compute the `pattern_id` its reply
  carries as information, and for a named statement it also calls
  `KeptParse::Of` (`src/server/kwp_session.cpp:667-680`).
- **`C_BIND` builds the text to execute.** For a statement with no kept
  parse it substitutes the values into the text (`Substitute`,
  `kwp_session.cpp:716-719`). The unnamed statement with no `?` binds to
  its own text unchanged. This is the text path `tools/kwp.py`'s
  `Connection.execute` drives (EA-S0's stamp).
- **The dispatcher parses that text again**: `HandleSelect`
  (`src/server/command_dispatcher.cpp:11262-11266`) and
  `ParseKeepingShape` for `INSERT`, `UPDATE` and `DELETE`
  (`command_dispatcher.cpp:8030-8050`). Each takes the fingerprint from its
  own lex.
- **Both fingerprints come from one implementation.** `FingerprintOf` is
  built on the accumulator the lexer feeds, so the two paths cannot give
  one statement two `pattern_id`s (`fingerprint.hpp:280-282`). A parse
  that stops early has fed only a prefix, and its fingerprint is not the
  statement's (`fingerprint.hpp:289-291`).
- **The parser's one input is the text**:
  `explicit Parser(std::string_view sql)` (`include/kds/parser/parser.hpp:27`).
  The review of this order found no global, thread-local, clock or
  configuration state in `src/parser/`: its `catalog::` uses are constants,
  and `AdmitHoles` is its one mode switch. S0 re-reads this (EM-S0 (ii)).
  The parser object carries one fact past a failed parse,
  `met_lock_clause()` (`command_dispatcher.cpp:11281`), so only a parse
  that succeeded is ever handed over.
- **The dispatcher parses the trimmed text.** `KeptFor` compares
  `Trim(line)` with `Trim(bound->text)` (`command_dispatcher.cpp:8022-8028`),
  and a refusal's byte is counted from the trimmed text. A parse at
  `C_PARSE` of the untrimmed text would shift every refusal byte by the
  leading whitespace.
- **One text is consumed more than once.** Within one dispatch, every
  re-run after a wait, a structural refusal or a pool refusal enters again
  with the same text ("every re-run is the same text",
  `command_dispatcher.cpp:1107`), and the bound statement stays on the
  session for the whole dispatch (`kwp_session.cpp:822-835`). Across
  dispatches, the unnamed statement lives until the next `C_PARSE`
  (`kwp_session.cpp:681-684`) and a new `C_BIND` runs it again. A parse
  handed over by move would reach the second consumer moved-from.
- **The compile changes the statement it is given.** `SelectBody` takes the
  run's statement by reference (`command_dispatcher.cpp:11405`), and view
  absorption replaces it and sets `views_settled`
  (`command_dispatcher.cpp:11492-11499`). An AST handed to one execution is
  therefore not a parser's output once that execution has compiled it.
- **A copy of a `Statement` shares its subqueries.** The `subquery`
  pointers are non-const `std::shared_ptr<SelectStmt>`
  (`include/kds/parser/ast.hpp:293`, `:1021`). That they are never written
  through is a comment (`ast.hpp:288-292`), kept today by copy-before-write
  in `Fill` (`src/parser/kept_parse.cpp:102-106`) and `StampCondition`
  (`src/exec/view_absorb.cpp:65-70`). Nothing in the types enforces it.
- **Other places that read the text again** (EA's census,
  `workorder-ea-prepared-statement-reuse.md:176-180`): `WithReturning`, the
  query layer, a multi-variant plan's arguments, a SET value's scalar
  subquery, and a re-run after a wait. A parked write resumes in its
  handler with its transaction, snapshot, cursor and binding carried, and
  no AST (`command_dispatcher.cpp:12436-12457`;
  `include/kds/server/session.hpp:384-416`). The resume is matched to its
  park by `is_delete` alone (`command_dispatcher.cpp:12436`). Every other
  park re-runs the statement whole (`session.hpp:379-381`), and that also
  parses again.
- **A multi-variant plan lexes the statement again at every compile** to
  read `ARG(k)` (`src/exec/admin_plans.cpp:47-49`, `parser::ArgumentsOf`).
  A statement from a kept parse does not (`known-gaps.md:3045-3051`). A
  variant is "declined, never forced" (`src/exec/step_compiler.cpp:3165-3175`;
  `docs/spec/parser-v2.md:142`), so a wrong variant runs another join
  order, access arm or build over the same rows. Under a `LIMIT` with no
  `ORDER BY` it may emit different rows, which the emission-order rule
  permits. `ANALYZE`'s plan line and the plan counters would name the
  wrong variant; that changes a report, not a row.

### 1.2 Name resolution of a qualified relation `[source-read]`

- **A relation's name is cached.** It is cached per core in a two-level
  map, namespace oid, then name, then oid. Only names that were found are
  cached, and the cache is dropped whole when the instance schema word
  moves at a task boundary (`catalog_cache.hpp:15-70`, `:96-99`, `:289-292`;
  `catalog.cpp:1146-1157`, `Revalidate`; `:1717-1746`).
- **A namespace's name is not cached.** `FindNamespaceOidByName` answers
  `public` and `sys` from constants and runs `ScanAll<SysObjectRow>` over
  `sys.objects` for every other qualifier (`src/catalog/catalog.cpp:2317-2338`).
  It is reached from `ResolveRelation` for every `ns.t` a statement writes
  (`catalog.cpp:2355-2370`), and six sites in `src/` call it
  (`catalog.cpp:2365`, `:4274`; `command_dispatcher.cpp:5618`, `:6610`,
  `:7018`, `:15694`).
- **Its stated reason is the one `FindTableOidByName` gives for not caching
  an absence**: "a caller looking a name up *expecting* NotFound before
  creating it" (`catalog.cpp:2335-2337`). `FindTableOidByName` does cache a
  name it found (`catalog.cpp:1738`).
- **What makes a positive namespace cache safe, as read:**
  - a namespace's oid comes from `GenerateUserOid` (`catalog.cpp:2469`)
    and is never issued again;
  - `DROP NAMESPACE` is RESTRICT (`namespace.md`) and calls
    `BumpVersion("drop namespace")` (`catalog.cpp:2547`);
  - CT7 lets no relation land in a dropped namespace;
  - no rename of a namespace exists (no `RenameNamespace` in `src/` or
    `include/`).

  A stale entry could therefore name only a namespace that no relation is
  in, which gives a false `NotFound` and never another relation. The
  review traced `ResolveRelation` to the same end: the namespace oid only
  keys a second lookup that still honours the view.
- **A qualified name can pay two scans.** While any view exists,
  `ResolveView` reaches `FindNamespaceOidByName` too (`catalog.cpp:4274`).
  The header of `catalog_cache.hpp` calls an uncached catalog scan "the
  dominant server-side cost of a statement" (`:15-20`).
- **The reserved spellings are answered before any lookup** "Fail towards
  the fixed answer" (`catalog.cpp:2319-2326`). A cache must sit after them.

### 1.3 What was measured `[measured]`

All figures are server CPU per statement, `build-release`, `cores = 1`.

- **EA-S0** (`bench/v3.4.17/results-ea-s0-premise-v3.4.17-202-g481d101aa.md`):

  | shape (text path) | parse | of server CPU | the second lex (`fp`) |
  |---|---|---|---|
  | point `SELECT` | 0.75 µs | 6.39 µs | 0.23 µs |
  | point `UPDATE` | 0.75 µs | 6.71 µs | 0.23 µs |
  | three-relation join | 1.87 µs | 10.59 µs | 0.86 µs |

  The second lex is 3.4 % to 8.1 % of a text statement's server CPU.
- **EA's close** (`bench/v3.4.17/results-ea-close-v3.4.17-205-g2c5c195f7.md`
  §2): a kept parse saves 0.47 to 0.55 µs on a point statement and 1.71 to
  1.73 µs on the join. The 0.22 to 0.29 µs it does not save on a point
  statement is the bind's AST copy and fill and the substitution. A text
  point `SELECT` is 6.415 µs before EA and 6.460 µs at its close (run 2).
- **Not measured anywhere**: a qualified name's namespace scan; a
  multi-variant plan's second lex on its own; how often a parked statement
  re-parses, and what it costs; how often a text statement repeats with
  identical literals; what a copy of a zero-hole parse (`Materialize`)
  costs.
- **C1's figure is gross.** The second lex it removes is 0.23 µs on a point
  statement, 3.4 % to 3.6 % of its server CPU, just over EM-Q1's 3 %. A
  copy per consumer (EM-R3) takes some of it back. Estimate: the net saving
  on a point statement is well under 0.23 µs, possibly near zero (basis:
  EA's 0.22 to 0.29 µs residual is mostly AST copy and fill; confidence:
  low). G0 judges the net figure, not the gross one.

## 2. Candidates `[design]`

Each candidate is named with its worst failure under a defect of its own
(§0's test) and the proof obligation that S0 must find true in the tree.

| | candidate | removes | worst failure | proof obligation |
|---|---|---|---|---|
| **C1** | **One lex for the unnamed text statement.** For an unnamed `C_PARSE` with no `?`, a DML statement's trimmed text is parsed once at `C_PARSE`. A parse that succeeded and reached the end of the statement gives the reply's `pattern_id` and is kept, and every consumer copies it out (EM-Q8 (a): a zero-hole `KeptParse`, the path EA reviewed). Where the parse fails or the statement is not DML, `FingerprintOf` runs as today and nothing is kept | the second lex, 0.23 to 0.86 µs gross `[measured]`, less the copy (§1.3) | a debug abort on a text mismatch | parse output depends on the text alone; the text parsed is `Trim`-equal to every consumer's (`KeptFor`'s test); no consumer gets a moved-from or a compiled AST (§1.1); a statement the query layer answers does not pay a parse it did not pay before (EM-R7) |
| **C2** | **A parked statement's parse kept across its park.** A write resumed after a wait or a DT slice takes a copy of the parser's output carried on `ParkedWrite`, in place of parsing its text again | one parse per resume, not measured. Estimate: under the bar against a 64-page slice (confidence: low) | a debug abort; a refusal | C1's; the copy taken before the first compile; the resume's text compared whole with the parked text, since the park is matched by `is_delete` alone (§1.1); the subquery pointers made `shared_ptr<const SelectStmt>` (EM-R3) |
| **C3** | **A multi-variant plan's arguments taken from the parse.** On the text path the parse collects `ARG(k)`, as a kept parse's arguments already serve one (EA-S2) | a lex per compile under a multi-variant plan, not measured | another variant: the same rows, a different `ANALYZE` line (§1.1) | a pin that the arguments the parse collects equal `ArgumentsOf(text)` over the golden corpus and the plan tests; a debug build cross-checks every compile |
| **C4** | **A positive namespace name to oid cache.** Kept in `CatalogCache` after the reserved spellings and dropped with the rest at the schema word; read and filled only on the unfiltered route, as `FindTableOidByName` is | one or two `sys.objects` scans per qualifier per compile (§1.2), not measured | a false `NotFound` | §1.2's facts; every namespace DDL either bumps the version or creates only, and a create invalidates no positive entry; for each of the six callers, that a stale entry ends in a refusal (EM-Q9) |
| **C5** | **A per-core parse cache keyed by the whole text** (explored; built only on S0's figure). Byte-exact text and the parser entry point map to the parser's output, which is copied out per execution | lex and parse on an identical-literal repeat | none beyond C1's | whole-text compare, with the entry point in the key (`Parse`, `ParseQuery` (`command_dispatcher.cpp:13479`), `AdmitHoles` and a view body's parse give different outputs for one text); C2's const subqueries, since a cached copy is shared across sessions; bounded memory; built only if S0 finds a shape class with identical-literal repeats whose saving clears the copy |

## 3. Rules

- **EM-R1. Every answer, every refusal and every refusal's byte position is
  what the statement gave before EM.** This includes the reply's
  `pattern_id`, and `fetch_id` and `arg_hash` in `sys.statement_stats`. The
  golden corpus pins the fingerprint, and `kFingerprintVersion` does not
  move.
- **EM-R2. Nothing is served by a hash alone.** A stored parse, argument
  list or oid is served only after its key is compared whole: the trimmed
  text for C1 and C2, the text and the parser entry point for C5, the
  namespace name for C4.
- **EM-R3. A reused parse is the parser's output.** It is copied out for
  each consumer, never moved, since one text is consumed more than once
  (§1.1). Nothing reused has been through a compile (view absorption).
  Before C2 or C5 lands, the AST's `subquery` pointers become
  `std::shared_ptr<const SelectStmt>`, so the compiler, not a comment,
  forbids a write through a shared subtree. Serving an AST that a compile
  or a write through a shared subtree has changed is `[quiet-wrong]`.
- **EM-R4. Nothing reused holds catalog state** beyond what
  `CatalogCache` already holds under the schema-word drop. C4's map joins
  that cache under the same rule; C1, C2, C3 and C5 hold none, and names are
  resolved again at every compile, as a kept parse's are (EA §2.4).
- **EM-R5. Where a candidate does not apply, the path is today's**, run
  byte for byte: a statement with `?` on the unnamed statement, a
  non-DML statement, a parse that fails, text over 64 KiB
  (`KeptParse::kMaxText`, `include/kds/parser/kept_parse.hpp:86`), the
  newline debug protocol.
- **EM-R6a. The CRUD weights are a direction** (`CLAUDE.md`'s working rules:
  read 45, update 25, insert 20, delete 10). S0 measures a text and a
  prepared shape of each kind, and the stages are ordered by G1's saving
  read against that mix. No candidate passes or fails on a weighted figure.
- **EM-R6. Measured in server CPU**, interleaved A/B, in `build-release`,
  with the server pinned to P-cores (CPUs 0-7 on this host), the client on
  another core, the data on a non-tmpfs device, and a floor run of A
  against A. Each stage runs within 20 minutes and the close within about
  an hour; a hold by another session's build or suite is outside the
  budget and is stated.
- **EM-R7. The text path is never made slower.** Each stage measures a
  text point `SELECT`, a text point `UPDATE` and a text `SELECT` the query
  layer answers interleaved at its end. The last one is there because C1
  would add a native parse at `C_PARSE` to a statement the native grammar
  declines. A lean that the floor does not exclude is reported as such.
- **EM-R8. A proof of concept is a spike.** It is built on the work branch,
  its binary hashed, and it is never merged as code. Only a candidate
  re-built at its own implementation stage lands.
- The standing rules, as DT's order lists them: a `critics-developer`
  review per step, the full suite per step, the contract suites (waystone,
  index, cabin, inner-build) green, and nothing pushed unasked.

## 4. Gates

| gate | after | passes when | if it fails |
|---|---|---|---|
| **G0 - admission** | S0 | the candidate's proof obligation (§2) is true as read at S0's commit, **and** its removable cost reaches EM-Q1's bar on at least one shape it serves | the candidate is dropped, with the reason in §8 |
| **G1 - proof of concept** | S1 | the spike's equality harness passes with no difference (§6's EM-S1), the suite runs green on the spike, and the spike's saving is resolved outside the A2-A floor and reaches the bar | not implemented; the figure goes in §8 |
| **G2 - implementation** | each S2.. stage | the full suite and the four contract suites pass, the equality harness is kept as a test, the review's findings are applied or rejected with a reason, and EM-R7's pair is measured | the stage does not land; the order stops there for the operator |
| **G3 - close** | Sz | overhead measured over the whole change, the docs synced (§6's Sz) | EM does not close |

## 5. Items for the operator (EM-Q), CLA's proposals

| | item | options | CLA proposes |
|---|---|---|---|
| EM-Q0 | Is EM opened, and with what scope? | (a) S0, then S1 for every candidate G0 admits, then one implementation stage per candidate G1 passes; (b) S0 alone | (a) |
| EM-Q1 | The bar | (a) the net removable cost is at least 3 % of the shape's server CPU, EA-Q1's line, on a shape the candidate serves; for C2 the denominator is the resumed slice's server CPU; (b) 1 %; (c) no bar, every admitted candidate built | (a) |
| EM-Q2 | The admission test | (a) §0's test: by construction, the worst failure is a refusal, a slower plan or a debug abort; (b) (a), and also a candidate whose only wrong answer needs a 64-bit hash collision | (a). (b) admits the excluded auto-prepare of §0.1 |
| EM-Q3 | EA-R4 ("A statement that is not prepared executes no new instruction") | (a) amended by EM for the unnamed no-`?` DML statement only, with EM-R1 as the condition and EM-R7 as the cost check; (b) kept, so C1 and C5 are not built | (a) |
| EM-Q4 | The `pattern_id` in `C_PARSE`'s reply | (a) the parse's fingerprint where the parse reached the end of the statement, and `FingerprintOf` otherwise; the reply is unchanged on the wire; (b) a protocol change that defers it to `C_EXECUTE` | (a). (b) moves a pinned frame |
| EM-Q5 | C4 inside EM? | (a) inside: it is the front end's name resolution, found in the session that asked for EM; (b) its own order | (a) |
| EM-Q6 | C3 inside EM? | (a) inside, under EM-R2 and its debug cross-check; (b) outside: its worst failure is a plan, not an answer, but the operator may rank a plan contract with answers | (a) |
| EM-Q7 | C5 | (a) explored at S0 only. A census of identical-literal repeats over the scenario drivers and the BS corpus decides it, and it is built only through G0 and G1; (b) not explored | (a) |
| EM-Q8 | C1's mechanism | (a) a zero-hole `KeptParse` for the unnamed no-`?` statement, copied out per consumer by `Materialize`: EA's reviewed path, with its `!name->empty()` test (`kwp_session.cpp:676`) relaxed for no-`?` text; (b) a take-once slot cleared with `std::exchange`, with every consumer after the first parsing as today | (a). (b) saves the copy for the first consumer only, and a missed clear hands a moved-from AST on |
| EM-Q9 | C4's callers | (a) every one of the six callers reads the cache once S0 has shown, per caller, that a stale entry ends in a refusal; (b) `ResolveRelation` and `ResolveView` only, the DDL and `USE` callers scanning as today | (b). The two resolution callers are the hot ones, and the DDL callers keep their unfiltered collision checks unchanged |

## 6. Stages

| stage | what | ends in |
|---|---|---|
| **EM-S0** | **(i) Census**: every parse site and every place the text is read again, EA's list re-verified at `main`, the whole-statement re-runs (`session.hpp:379-381`) included; every consumer of one text within one dispatch and across re-binds of the unnamed statement; every caller of `FindNamespaceOidByName` and the route each takes. **(ii) Proof obligations**: whether any session, instance or configuration state reaches a DML parse (a re-read of `src/parser/`); every compile site that changes its statement; that no code writes through a shared `subquery` subtree; every namespace DDL and whether it bumps the version; the oid issue. **(iii) Premise**: counters spiked around the lex, the parse, the fingerprint, `FindNamespaceOidByName` and `ArgumentsOf`, never committed, over a text and a prepared point `SELECT`, point `UPDATE`, point `INSERT`, point `DELETE` and three-relation join, a qualified `ns.t` beside the same `t` under `USE` with and without a view in the instance, a two-variant plan, and a walked `UPDATE` that parks and resumes; `Materialize` of a zero-hole parse for the same shapes, which gives C1's net figure; for C5, a repeat census by `(fetch_id, arg_hash)`. **(iv)** Whether a prepared and a text statement with leading whitespace name the same refusal byte (§8, found by the review) | the census and the obligations in §8; each candidate's figure against EM-Q1's bar; G0's admitted list |
| **EM-S1** | **Proof of concept**, one spike per admitted candidate, with an equality harness: **C1, C2, C5**: for every statement in `parser_golden_test` and `kept_parse_test`'s corpora, the reused parse equals a fresh `Parse()` of the text, its fingerprint triple equals the fresh one, and every refusal names the same byte. **C3**: the parse's arguments equal `ArgumentsOf(text)` over the same corpora and every `tests/*plan*` statement. **C4**: a cached resolution equals the scan under an interleaving of `CREATE NAMESPACE`, `DROP NAMESPACE` and `CREATE TABLE` across two cores and the DDL-view route, with `sim/` crashed mid-DDL. The saving is measured as EM-R6 | G1's verdict per candidate; the figures in §8 |
| **EM-S2..** | **Implementation**, one candidate a stage, in the order of G1's measured saving; the harness becomes a test | G2 per stage |
| **EM-Sz** | **The close**: the overhead over the whole change, the commit EM opened from against its last stage; `protocol.md` §5 (C1, EM-Q3's amendment of EA-R4), `parser-v2.md` (the parse sites), `catalog.md` and `catalog_cache.hpp`'s header (C4), `known-gaps.md` (`:1733-1736` and `:3045-3051` rewritten or removed), `CLAUDE.md`'s KWP row, `milestones.md` | G3; the operator's word on landing |

## 7. What EM does not do

- Nothing §0.1 excludes. C5's per-core cache is the only parse reused
  across sessions, and only if G0 and G1 pass.
- No protocol change: every frame and reply is byte-identical (EM-Q4 (a)).
- No change to which plan or which refusal a statement gets, and none to
  the fingerprint.
- No unification of the query layer's grammar with the native one: its
  second read of the text stays.
- No change to NS6: names stay unique within a namespace.
- Nothing of the compile's own cost beyond C3's lex. The two compiles of a
  reordered block stay (`known-gaps.md:3058`).

## 8. Stage log (CLA, appended as stages land)

### Written 2026-11-28

On `front-end-parse-reuse-order` from `72580aa04`, on the operator's word.
It was written as EI. The sync with `origin/main` at `1983ae5a7` found
EI to EL already written by another session from `72580aa04`, so this
order took the next free letter, EM, before it landed.
No stage is opened and no code is written. The candidates come from a
reading of the front end done in the session that asked for EM: C1 and C3
are gaps already in `known-gaps.md`, C2 covers the resumed write of EA's
census item "a re-run after a wait" (a whole-statement re-run is not
covered by any candidate), C4 is the uncached namespace scan found in that
reading (§1.2), and C5 is the whole-text cache kept to S0 by EM-Q7. Only
C1's gross cost is measured, at EA-S0.

**The `critics-developer` review of the written order** checked every
citation at `72580aa04`. It corrected twelve line numbers, figures and
quotations in place. Among them, §1.3's table had mixed EA-S0's prepared
parse with the text path's second lex, and now shows the text rows. It
found that C1 as first written did not pass §0's test. C1 handed its
parse "by move" to "its one execution", but one text is consumed several
times within a dispatch and across re-binds, so a later consumer would get
a moved-from AST. C1 now copies per consumer (EM-R3, EM-Q8), parses the
trimmed text and keeps only a parse that succeeded. Its figure is now
gross, with G0 judging the net (§1.3). The review also found:

- the shared non-const subquery pointers, which made EM-R3 a convention
  for C2 and C5;
- the parser entry point missing from C5's key;
- C4's reserved-spelling order and caller scope (EM-Q9);
- `ResolveView`'s second scan;
- an incoherent lead line in §0.1;
- the query-layer statement missing from EM-R7.

All of these are taken. **Found on the way, not verified by a run**:
EA's `KeptParse::Of(stmt.sql)` parses the untrimmed text
(`kwp_session.cpp:677`), where the dispatcher parses the trimmed one. A
prepared and a text statement with leading whitespace may therefore name
different refusal bytes. EA-R3 permits this, but the two paths diverge.
S0 (iv) runs the check. Rejected: none.
