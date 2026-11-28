# Work order EM - the statement front end's repeated work, cut only where no cut can change an answer

Status: **OPENED 2026-11-28 on the operator's standing go-ahead
(go-ahead-achieving-milestone, `raft-marks-2026-11-28.md` §22), on worktree
`em-front-end-reuse` (branch `worktree-em-front-end-reuse`) from `b46ac5140`
(`v3.5.1-69-gb46ac5140`).** EM-Q0..Q9 are adopted as CLA proposed, and so
are EM-Q10 and EM-Q11, written at S0. Each is CLA's proposal, adopted under
that word, and none is a per-item mark by the operator (§5's mark column).
**CLOSED 2026-11-28 on the work branch**: C4 and C3 are built (EM-S2a, EM-S2b); C1 failed G1, and C2 and C5 were dropped at G0. The close measured EM-R7 as a lean of up to +1.5 % that the floor does not exclude (§8, EM-Sz). Not merged to `main`.
Written on worktree `front-end-parse-reuse-order` from `72580aa04`
(`v3.5.1-6-g72580aa04`) on the operator's word of 2026-11-28
(`raft-marks-2026-11-28.md` §8). The `[source-read]` claims in §1 were
read by CLA at `72580aa04` and re-read at S0 at `b46ac5140` (§8). A claim
marked "EA's census" was read by EA's session at `481d101aa` and is
re-verified at S0.

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

| | item | options | CLA proposes | mark |
|---|---|---|---|---|
| EM-Q0 | Is EM opened, and with what scope? | (a) S0, then S1 for every candidate G0 admits, then one implementation stage per candidate G1 passes; (b) S0 alone | (a) | (a), adopted on the operator's standing go-ahead of 2026-11-28 (go-ahead-achieving-milestone) |
| EM-Q1 | The bar | (a) the net removable cost is at least 3 % of the shape's server CPU, EA-Q1's line, on a shape the candidate serves; for C2 the denominator is the resumed slice's server CPU; (b) 1 %; (c) no bar, every admitted candidate built | (a) | (a), adopted on the operator's standing go-ahead of 2026-11-28 (go-ahead-achieving-milestone) |
| EM-Q2 | The admission test | (a) §0's test: by construction, the worst failure is a refusal, a slower plan or a debug abort; (b) (a), and also a candidate whose only wrong answer needs a 64-bit hash collision | (a). (b) admits the excluded auto-prepare of §0.1 | (a), adopted on the operator's standing go-ahead of 2026-11-28 (go-ahead-achieving-milestone) |
| EM-Q3 | EA-R4 ("A statement that is not prepared executes no new instruction") | (a) amended by EM for the unnamed no-`?` DML statement only, with EM-R1 as the condition and EM-R7 as the cost check; (b) kept, so C1 and C5 are not built | (a) | (a), adopted on the operator's standing go-ahead of 2026-11-28 (go-ahead-achieving-milestone) |
| EM-Q4 | The `pattern_id` in `C_PARSE`'s reply | (a) the parse's fingerprint where the parse reached the end of the statement, and `FingerprintOf` otherwise; the reply is unchanged on the wire; (b) a protocol change that defers it to `C_EXECUTE` | (a). (b) moves a pinned frame | (a), adopted on the operator's standing go-ahead of 2026-11-28 (go-ahead-achieving-milestone) |
| EM-Q5 | C4 inside EM? | (a) inside: it is the front end's name resolution, found in the session that asked for EM; (b) its own order | (a) | (a), adopted on the operator's standing go-ahead of 2026-11-28 (go-ahead-achieving-milestone) |
| EM-Q6 | C3 inside EM? | (a) inside, under EM-R2 and its debug cross-check; (b) outside: its worst failure is a plan, not an answer, but the operator may rank a plan contract with answers | (a) | (a), adopted on the operator's standing go-ahead of 2026-11-28 (go-ahead-achieving-milestone) |
| EM-Q7 | C5 | (a) explored at S0 only. A census of identical-literal repeats over the scenario drivers and the BS corpus decides it, and it is built only through G0 and G1; (b) not explored | (a) | (a), adopted on the operator's standing go-ahead of 2026-11-28 (go-ahead-achieving-milestone); C5 dropped at G0 (§8) |
| EM-Q8 | C1's mechanism | (a) a zero-hole `KeptParse` for the unnamed no-`?` statement, copied out per consumer by `Materialize`: EA's reviewed path, with its `!name->empty()` test (`kwp_session.cpp:676`) relaxed for no-`?` text; (b) a take-once slot cleared with `std::exchange`, with every consumer after the first parsing as today | (a). (b) saves the copy for the first consumer only, and a missed clear hands a moved-from AST on | (a) as revised at S0 (§8, EM-Q11), adopted on the operator's standing go-ahead of 2026-11-28 (go-ahead-achieving-milestone) |
| EM-Q9 | C4's callers | (a) every one of the six callers reads the cache once S0 has shown, per caller, that a stale entry ends in a refusal; (b) `ResolveRelation` and `ResolveView` only, the DDL and `USE` callers scanning as today | (b). The two resolution callers are the hot ones, and the DDL callers keep their unfiltered collision checks unchanged | (b), adopted on the operator's standing go-ahead of 2026-11-28 (go-ahead-achieving-milestone) |
| EM-Q10 | C1's text whose parse fails (written at S0) | (a) the failure is kept beside the statement - its refusal, the parser's lock-clause fact and its fingerprint, each a function of the trimmed text alone - and the dispatcher serves it in place of a native parse of the same text that would fail the same way; (b) nothing is kept, and a statement the native grammar declines pays one native parse more, at `C_PARSE`; (c) C1 is not built | (a). (b) breaks EM-R7 by construction: the query-layer `SELECT` would pay a second failed native parse, one of the 3 parse calls its 1.59 µs of parsing is (§8; the failed one's share not measured). (a) moves the failed parse from the dispatcher to `C_PARSE`, so the statement pays what it pays today | (a), adopted on the operator's standing go-ahead of 2026-11-28 (go-ahead-achieving-milestone) |
| EM-Q11 | C1's zero-hole parse (written at S0) | (a) a statement with no `?` is parsed once, with no `HolesDelimited` scan and no hole walk (no `?` means no hole, so both answer true by construction), its fingerprint taken from the parse, and its arguments collected by the same parse (C3's mechanism, so C3 lands first); (b) EA's `KeptParse::Of` as it stands | (a). (b) lexes the text three times to remove one lex: S0 measured it 0.33 to 0.76 µs dearer than the text path it replaces (§8) | (a), adopted on the operator's standing go-ahead of 2026-11-28 (go-ahead-achieving-milestone) |
| EM-Q12 `[quiet-wrong]` | C4's stale entry within a statement (written at S1) | (a) the namespace map is read and filled only while `Catalog::MemoIsCurrent()` holds, so no entry served is older than the scan it replaces; (b) C4 as first written, its stale entry reaching the relation cache's stale entry as an unqualified name already can (the review's restatement); (c) C4 dropped | (a). (b) adds a window to qualified names that today scan fresh, and that window's outcome, a dropped relation's rows or a refusal, is unsettled (§8). (a) costs one acquire load a lookup | (a), adopted on the operator's standing go-ahead of 2026-11-28 (go-ahead-achieving-milestone); its premise re-read at `b46ac5140` (§8) |

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

### Opened 2026-11-28

On the operator's standing go-ahead (`/go-ahead-achieving-milestone EM`,
`raft-marks-2026-11-28.md` §22), on worktree `em-front-end-reuse` from
`b46ac5140` (`v3.5.1-69-gb46ac5140`), which is `origin/main`. EM-Q0..Q9 are
adopted as CLA proposed: (a) for each, and (b) for EM-Q9. None is
`[quiet-wrong]`-tagged. EM-Q2 is the item a wrong choice would turn into
one, because (b) admits a hash-collision answer. Its premise, that §0.1's
exclusions are the only candidates a hash could serve, was re-read
against the tree before the adoption and holds: C1 to C5 each compare their
key whole (EM-R2).

### EM-S0 - census, proof obligations, premise; G0 (2026-11-28)

On `em-front-end-reuse` at `b46ac5140`. The measurement is
`bench/v3.5.1/results-em-s0-premise-v3.5.1-69-gb46ac5140.md`; the figures
below are its medians.

**(i) Census `[source-read]`.** Since `72580aa04`, of the files §1 cites,
three moved. `command_dispatcher.cpp` (144 lines in, 32 out): §1's numbers
below `:7756` stand, `:8022-8050` are 79 low, `:11262-11499` 90 low and
those past `:11716` 112 low. `known-gaps.md` (110 in): every line this
order cites in it is 110 low (`:1843-1846`, `:3155-3161`, `:3168`,
`:3171-3172`). `command_dispatcher.hpp` (15 in) is cited at `b46ac5140`
only. The rest still stand. Every parse
site in `src/` outside the parser:

- **DML**, one parse a statement: `HandleSelect` (`command_dispatcher.cpp:11355`),
  and `ParseKeepingShape` (`:8109`) for `InsertInner` (`:8134`),
  `UpdateInner` (`:13644`), `DeleteInner` (`:18461`) and `WithReturning`
  (`:12979`, a second parse of a write with `RETURNING`).
- **Read again from text**: `TryQueryLayer` (`:13590`), `RunScalarSubquery`
  (`:12878`), `DescribeBody` (`:13216`), a view body (`exec/view_absorb.cpp:120`),
  a multi-variant plan's `ArgumentsOf` (`exec/admin_plans.cpp:47`), and the
  whole-statement re-runs (`session.hpp:379-381`).
- **DDL and utility** (`HandleIndex`, `HandleCabin`, `HandleAlter`,
  `HandleDropTable`, `HandleNamespace`, `HandleAssertion`,
  `HandleTransition`, `HandleCreateTableSql`, `HandleView`,
  `HandleWaystone`, `HandleCreatePlan`, `HandleAdvisePlan`, `HandlePurge`;
  `exec/transition.cpp:202`, `exec/plan_catalog.cpp:616`). None of these is a
  candidate's site.

`FindNamespaceOidByName`'s six callers: `ResolveRelation`
(`catalog.cpp:2365`), `ResolveView` (`:4274`), and the dispatcher's
`:5618` (`HandleNamespace`), `:6610`, `:7018` (a foreign key's
`references_schema`) and `:15806`.

**(ii) Proof obligations `[source-read]`.**

- **The parse depends on the text alone.** `src/parser/` holds no
  thread-local, static mutable, clock, environment or configuration state.
  Its `catalog::` uses are constants. Its one mode switch, `AdmitHoles`,
  acts only on a `kParam` token (`parser.cpp:323`).
- **A parse that succeeded reached the end of the statement.** After a
  successful parse only `kEof` may remain (`parser.cpp:4047-4050`), so the
  fingerprint accumulator has been fed `kEof`. The parse's fingerprint is
  therefore `FingerprintOf`'s, which is built on the same accumulator
  (`fingerprint.hpp:280-282`).
- **No `?` in C1's text.** `PlaceholderCount` is a byte scan, not a lex
  (`kwp_session.cpp:268-274` over `ScanPlaceholders`, `:152-176`). C1 therefore parses with a plain `Parser`,
  no `AdmitHoles`, the constructor and text the dispatcher uses, so a `?`
  the scan misses is refused by both parses in the same words.
- **The compile edits its statement.** It still does (`SelectBody`;
  `AbsorbViews`), and C1 copies out per consumer (`Materialize`), as EA
  does.
- **No write through a shared subtree.** No write through a shared
  `subquery` was found beyond EA's copy-before-write sites
  (`kept_parse.cpp:102-106`, `view_absorb.cpp:65-70`). EM-R3's
  `shared_ptr<const SelectStmt>` binds C2 and C5, which G0 drops.
- **Namespace DDL.** A namespace's oid is `GenerateUserOid`'s
  (`catalog.cpp:2469`), never issued again. `DropNamespace` bumps the
  version (`:2547`). `CreateNamespace` bumps it too, through
  `InsertObjectRow`'s `sys.objects insert` (`:1299`; correction at S2a's
  review, where this line read "does not"), and C4 needs no bump from it: it
  refuses a live name (`CheckNameFree`), so a name a positive entry holds
  is free only after a drop, which bumped. No rename exists.
- **The route C4 sits on.** Every resolution goes through `ResolveNames`
  (`command_dispatcher.hpp:1415-1458`). It hands the catalog no view, the
  unfiltered route, unless some transaction holds uncommitted DDL, and
  re-resolves under a minted view when a DDL opened meanwhile. A cache read
  and filled only on that route, as `FindTableOidByName`'s is
  (`catalog.cpp:1717-1746`), serves no uncommitted namespace: a fill can
  race an opening DDL, but `ViewFor` mints a view while any DDL is open and
  both endings move the word (`EndDdlScopeById`,
  `command_dispatcher.cpp:10430-10438`).
- **Correction** (found by CLA at S1 and by the stage's review, each
  independently). The line that stood here, "a stale entry can name only a
  namespace dropped within the core's current task, which RESTRICT and CT7
  keep empty, so the relation lookup under it ends in `NotFound`", is
  false. The memo is checked at the statement's head only
  (`command_dispatcher.cpp:1372-1375`). Between core A's head and A's
  resolution, core B can drop `ns1.t` and `ns1` and create both again. A
  cached `ns1 -> X` then leads to the relation cache's `(X, t)`, the dropped
  relation's oid, where today's scan answers the new `t`. RESTRICT keeps
  `X` empty at its drop, not at A's head. C4 as first written would
  therefore extend to qualified names the window `known-gaps.md` records
  for a cached relation name (the head's reading, "Not reproduced"), and
  whether that window can give a wrong row is not settled.
  **EM-Q12 (a) closes it for C4**: the map is read and filled only while
  `Catalog::MemoIsCurrent()` holds (`catalog.hpp:317`, the check a writer's
  first `IX` makes). An entry served is then no older than the scan it
  replaces, so it names what that scan would.

**(iii) Premise `[measured]`.** As a share of the shape's server CPU,
C1's net removable cost is 1.3 % to 1.8 % on the four point shapes and
6.4 % on the three-relation join. C2's is 0.02 % of a slice: about 3.3 µs
a re-parse against about 20 ms a slice, 6 parses over a walked `UPDATE` of
60,000 rows. C3's is 5.7 % on the join under a two-variant plan, gross.
C4's is 5.9 % on `ns1.t2`, 10.2 % with one view in the instance (two
scans) and 29.1 % with the view and 100 more relations, because the scan
reads every `sys.objects` row. **C1 through today's `KeptParse::Of` would
cost more than it removes**: at `C_PARSE` it lexes three times
(`HolesDelimited`, the parse, `ArgumentsOf`) and walks for holes, 1.08 to
3.44 µs, and net of the parse it moves and of the lex it removes it is
0.33 to 0.42 µs dearer than today's text path on a point shape and
0.76 µs on the join. EM-Q11 therefore states C1's zero-hole path. **The
query-layer `SELECT` (EM-R7)** parses 3 times (1.59 µs). Under
C1 as first written, a native parse that fails would run once more, at
`C_PARSE`. EM-Q10 keeps the failure instead.

**C5's census.** `tools/eqbench` times every DML statement as a named
statement with `?` holes, which EA's kept parse already serves. `sim/`
draws its literals from a seed. No shape class repeats its literals.

**(iv) The refusal byte** (§8, "Found on the way"): confirmed. With two
leading spaces, a named statement names a byte 2 higher than the same text
unnamed, in six of seven statements tried. This predates EM, and EM-R1
keeps it. It is recorded at
`docs/inflight/bugs/kept-parse-refusal-byte-counts-leading-whitespace.md`.
C1 parses `Trim(sql)`.

**G0.**

| candidate | obligation | bar | G0 |
|---|---|---|---|
| C1 | holds, with EM-Q10 (a) and EM-Q11 (a) | met on the join (6.4 %) | **admitted** |
| C2 | not examined further | 0.02 % | **dropped**: the parse is noise against a slice |
| C3 | holds, with a debug cross-check against `ArgumentsOf` | met (5.7 %, gross) | **admitted** |
| C4 | holds on `ResolveRelation` and `ResolveView` (EM-Q9 (b)) | met (5.9 % to 29.1 %) | **admitted** |
| C5 | - | no shape class | **dropped** |

**C1 as admitted.** At an unnamed `C_PARSE` whose text has no `?`, is at
most `KeptParse::kMaxText` and whose first word is `SELECT`, `INSERT`,
`UPDATE` or `DELETE`, the trimmed text is parsed once, by the parser the
dispatcher would use:

- **A DML parse that succeeds** is kept as a zero-hole `KeptParse`. Its
  fingerprint and arguments come from that parse (EM-Q11, C3's
  collection), and the reply's `pattern_id` is the parse's (EM-Q4 (a)).
- **One that fails** keeps its refusal, its lock-clause fact and its
  fingerprint (EM-Q10 (a)). `FingerprintOf` answers the reply as today.
- **Every other text** takes today's path, byte for byte (EM-R5).

**The stage order.** S1 spikes C4, C3 and C1 in one binary. They serve
disjoint cells (qualified names, plan-governed text, plain text), so one
interleaved A/B, against an A2 copy of A, attributes each. S2's order,
provisional until G1's figures (§6): C4 first (the largest saving), then
C3, then C1. C3 goes before C1 because C1's kept parse takes its arguments
from C3's collection, and that dependency, not their S0 savings (0.66 µs
and 0.68 µs, a tie; S0 ran no floor), puts C3 first.

**Review** (`critics-developer`, at the step's diff on `b46ac5140`). It
corrected in place the C4 percentages (now the scan over the qualified
cell, 5.9 % to 29.1 %), the `KeptParse::Of` comparison (0.33 to 0.76 µs
dearer, net of the parse it moves), the census's line drift, two
citations, and an unmeasured share stated as measured. Its other
findings:

- **Taken**: a kept failure is consulted only where the dispatcher would
  parse, and `C_BIND` runs `Substitute` exactly as today, so an
  unterminated string is still refused at `C_BIND` in today's words.
  C1's head test is the dispatcher's own routing (its first token, split as
  `SplitFirstToken` splits it, with `DELETE STATISTICS` and every utility
  form excluded), so no text the dispatcher does not route as DML pays a
  parse at `C_PARSE` (EM-R5).
- **Taken**: S1's C4 harness covers `DROP TABLE` and a re-create of the
  namespace and the relation between a head and a resolution.
- **Rejected**: the review's restatement of C4's proof as "a stale entry
  resolves as the session-namespace oid of `USE` already does". It is
  true of the code, but it admits into C4 a window whose outcome is not
  shown to be a refusal, so C4 would no longer pass §0's test by
  construction. EM-Q12 (a) gates the map instead.
- **Not taken**: the driver's two `USE` statements inside the timed `nsu`
  block, about 0.01 µs against C4's 0.48 µs, and its duplicated
  parts-delta loop. The run is archived as it ran, and the review found
  that no verdict changes.

### EM-S1 - the spikes; G1 (2026-11-28)

On `em-front-end-reuse` from `259a7fa3a`, the spikes applied to detached
copies of `b46ac5140`. The measurement is
`bench/v3.5.1/results-em-s1-poc-v3.5.1-70-g259a7fa3a.md`.

- **C4 passes G1**: −0.40 µs on `ns1.t2` (−5.7 %) and −0.77 µs with a view
  (−10.6 %), the A2 floor within ±0.4 %. EM-Q12 (a) is in the measured
  spike. Its harness cell is red against the spike without the gate:
  core 1 answers the dropped relation's oid, which measures EM-Q12's
  premise.
- **C3 passes G1**: −0.67 µs on the join under a two-variant plan
  (−5.7 %).
- **C4 and C3 together** lean +0.04 µs (+0.6 %, +0.7 %) on the point
  `SELECT`, text and prepared, and +0.03 µs (+0.4 %) on the query-layer
  `SELECT`. Each is outside its floor but inside A's spread, and is
  reported as a lean (EM-R7).
- **C1 fails G1, and EM-R7 refuses it.** It adds +0.36 to +0.45 µs to each
  of the four text point shapes and +0.41 µs to the query-layer `SELECT`, and saves
  0.08 µs on the join. The parse moved to `C_PARSE` runs 0.12 to 0.14 µs
  dearer, and the kept parse's allocations and copies cost more than the
  0.19 to 0.24 µs lex removed. **C1 is not implemented.** EM-Q3, EM-Q4,
  EM-Q10 and EM-Q11 therefore govern nothing built. They stand as adopted
  for the record, and EA-R4 stays unamended. Its equality harness (green)
  and the spike are archived as figures, not code (EM-R8).
- **A pre-existing window, measured at catalog level and not fixed**: an
  unqualified relation name resolved within a statement, after another
  core dropped and re-created it, answers the dropped relation's oid. This
  is now recorded in the existing `known-gaps.md` entry ("A DDL that opens
  and closes inside one statement..."), whose full close is unchanged.
  C4's gate keeps qualified names out of it.
- **The suite was not run on the spike trees.** C4's and C3's stages build
  the same code with the harness as tests, and the full suite gates each.

**The stages.** S2a builds C4, the larger saving. S2b builds C3. There is no
S2c.

**EM-S1 review** (`critics-developer`, at `cb768add9`). It recomputed every
cell of the results file's table from the raw runs and found them right,
and the G1 verdicts with them. It corrected the prose: C1's own figure is
ALL against C43 (+0.36 to +0.45 µs, +6.0 % to +6.9 % of A); the lex it
removes is 0.19 to 0.24 µs; the three-arm run's floor was not within
±0.4 %; and the leans of C4 with C3 include the query-layer `SELECT`
(+0.03 µs, +0.4 %). Applied at `60ad34bfe`, with the driver's docstring
and its rotation check. Not taken: none.

### EM-S2a - C4, the namespace map (2026-11-28)

On `em-front-end-reuse` from `60ad34bfe`: the S1 spike's C4, with EM-Q12
(a)'s gate (`Catalog::ResolveNamespace`, `CatalogCache::FindNamespaceOid`,
`PutNamespaceOid`, cleared by `Invalidate`). Its harness is kept as
`CatalogNamespaceCacheTest`, five cells. The first is red against the
gate's removal; the last asserts the namespace's own refusal. The
pre-existing window is now in `known-gaps.md`, at its existing entry.
Overhead not measured; measured at the milestone's close.

**Review** (`critics-developer`, the stage's diff). No correctness bug:
every path it traced answers what `FindNamespaceOidByName(name, view)`
would at that moment. These include another core's namespace or relation
DDL after a head, a fill racing a bump, an uncommitted create, a rolled-back
drop, the graveyard, `Bootstrap`, `DropCache` and a catalog with no
instance words. Its other findings:

- **Corrected**: S0's "`CreateNamespace` does not bump". It does,
  through `InsertObjectRow` (`catalog.cpp:1299`), which only strengthens
  the proof.
- **Applied**: two comments, the reasoning written once at
  `ResolveNamespace`'s body, and the weak last cell, which now asserts the
  message, so a map not cleared at the head turns it red.
- **Not taken**: none.

**Suite**: 5,442 of 5,443 at the stage's code before the review's
comment and test edits. The one failure is
`DropTableReclaimCrashTest.AReclaimCutAnywhereIsFinishedByTheNextMount` on
the `/tmp` quota held by other sessions' files, and it passes when rerun.
After the edits, `CatalogNamespaceCacheTest` passes. The full suite runs
again at S2b over both stages.

### EM-S2b - C3, a plan's arguments from the parse (2026-11-28)

On `em-front-end-reuse` from `e54ac0685`, the S1 spike's C3. The lexer
collects the arguments as it lexes, when asked (`Lexer::CollectArguments`,
fed in `ReadToken` beside the fingerprint, one `AppendArgument` shared with
`ArgumentsOf`). `HandleSelect` asks while an administrator's plan is
enabled, and `SelectBody` hands the compile the run's parse's arguments
where no kept parse does. A debug build compares them with `ArgumentsOf`
at every such compile and aborts on a difference. The writes'
`FindWritePlan` keeps its lex, which S0 did not measure. The harness is
kept as `KeptParseTest.ACollectingParseGivesArgumentsOfItsText`. A mutant
collector that dropped integer arguments aborted `admin_plans_test` at the
debug check, so the plan tests' own statements exercise it. Overhead not
measured; measured at the milestone's close.

**Review** (`critics-developer`, the stage's diff). No bug: it traced the
parser's text against `line`, a failed parse, the whole token stream of a
successful one, the DDL lexer copies, `ANALYZE`, a subquery, an absorbed
view, re-runs, the views' lifetime across yields, `any()` changing between
parse and compile, and `KeptFor` at both sites. **Applied**: a defaulted
`StatementArgument::operator==`, which replaces both comparison loops, and
the redundant `!line.empty()` dropped. **Not taken**: one accessor
fewer on the lexer (`collects_arguments`). It saves one line and moves the
lexer's public surface. **Noted for the close**: `ReadToken` now tests one
flag on every token of every lex, which EM-R7's measurement prices.

**Suite**: 5,442 of 5,444 before the review's cuts. The two failures are
`DropTableReclaimCrashTest`, on the `/tmp` quota (it passes rerun on disk),
and `DurableKickRigTest.APeersGroupCommitIsKickedByTheWriter`, the
two-core flake memory records at about 1 in 5 at base (4 of 5 rerun).
After the cuts, the parser, plan and kept-parse cells (195) pass. The
whole suite runs again at the close.

### EM-Sz - the close (2026-11-28)

On `em-front-end-reuse`, the code at `aa8eaebbc` (`v3.5.1-74-gaa8eaebbc`)
against `b46ac5140`, measured by `ck-tester`
(`bench/v3.5.1/results-em-close-v3.5.1-74-gaa8eaebbc.md`; two runs of 9
rounds, A2 a byte copy of A):

- **C4 holds its saving**: −0.38 µs on `ns1.t2` (−5.6 %), −0.71 µs with a
  view (−10.0 %), each more than nine times its cell's floor.
- **C3 holds its**: −0.5 to −0.9 µs (4 to 8 %) on the join under a
  two-variant plan. The direction holds in both runs, and the cell's floor
  is wide (−1.5 %, −4.0 %).
- **EM-R7 is not shown to hold to the floor.** The point `SELECT` leans
  +1.0 % text and +1.3 % prepared, `DELETE` +0.9 %, and `t2` under `USE`
  +1.5 %: +0.05 to +0.09 µs, each inside A's spread in one run or the
  other, with the sign repeating across both runs and EM-S1's C43 arm.
  The query-layer `SELECT`, `INSERT` and both joins are inside the floor.
  **CLA's reading, not measured**: the prepared point `SELECT` runs none of
  EM's code (no parse, no qualifier, no plan) and leans as much as the text
  cells, which points to code placement in the binary rather than to EM's
  work. No arm separates C3 from C4, so the lean is not attributed. It is
  reported as a lean, not as a pass and not as a separated regression.

**Documents.**

- `catalog.md` CT3 states C4.
- `parser-v2.md`'s plan paragraph states C3.
- `known-gaps.md`: the second-lex entry records C1's measured failure, the
  multi-variant entry narrows to writes, and the stale-memo entry records
  the pre-head form.
- `docs/inflight/bugs/kept-parse-refusal-byte-counts-leading-whitespace.md`
  is new.
- `CLAUDE.md`'s Namespaces and Query language rows.
- The SQL surface is unchanged, so the manual is too.
- `protocol.md` §5 is unchanged, since C1 was not built and EA-R4 stands.

**The sync.** `git fetch origin` found `origin/main` still at `b46ac5140`,
so there was nothing to merge.

**Achieved, narrowed.** Two of the five candidates are built:

| candidate | outcome |
|---|---|
| C2 | dropped at G0 |
| C5 | dropped at G0 |
| C1 | dropped at G1 |
| C3 | built |
| C4 | built |

**Suite at the close** (`aa8eaebbc`'s code): 5,442 of 5,444. The two
failures are `DropTableReclaimCrashTest`, on the `/tmp` quota held by other
sessions' files (it passes rerun on disk), and
`ContentionCountersTest.TwoCoresInsertingIntoOneRelationContendWhatAnInsertTakes`,
the two-core flake memory records (2 of 3 rerun). Neither is a green run
of the whole suite, and this log does not report one.
