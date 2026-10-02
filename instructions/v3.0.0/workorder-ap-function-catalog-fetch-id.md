# Work order AP — `fetch_id`, rule 0′ and the function catalog

Written 2026-10-02 on `worktree-ap-s0-order` from `4012617`
(`v2.7.0-573-g4012617`). The operator's word was *"b"*, answering CLA's
three options with *"Write AP's work order (AP-S0) from AR1 §14"*. AR1 is
ratified (`raft-marks-2026-09-29.md` §5), and AZ-Q5 settled AP first
(`raft-marks-2026-09-30.md` §16). AR1's status line says that settlement
*"opens no letter"*; the word above is what opens this one.

**This order is written, not opened.** Its §4 items are unmarked, and every
stage waits for its own word. It cuts no tag; the v3.0.0 tag waits on M4
(`raft-marks-2026-09-30.md` §16).

## 0. What AP is

AR1 §14 gives AP this content:

> function catalog, purity, determinism class, `fetch_id` + rule 0′,
> `SysPatternRow` change

AR1-V amends two of those items, and the ratification took the amendments
as the proposals:

- **Rule 0′ is a wording change**, because the hazard it closes does not
  exist (AR1-V1, D8).
- **AP's priority rests on having no dependency**, not on a hole in the
  engine (AR1-V2).

The ratified items AP builds against:

| AR1 item | what | class |
|---|---|---|
| AR1-1, §2 | the function catalog: name, arity, purity, evaluate, optional cover/monotone | design |
| AR1-2, §3 | a statement's determinism class D0/D1/D2, folded at parse from predicate-position purity | design |
| AR1-3, §4 | `fetch_id`, a second shape hash without the select list; trails key on it, statistics keep `pattern_id`; `kFingerprintVersion` does not move | design |
| D2 | an undeclared function's purity is `kVolatileRow` | **quiet-wrong**, ratified |
| D6 | `fetch_id` on `SysPatternRow`; `waystone_root` re-keyed; "one row change, no `kFingerprintVersion` bump" | format, ratified |
| D8 | rule 0′, as a wording change to rule 0 (AR1-V1) | **quiet-wrong**, ratified |

**These belong to AQ, not AP:**

- `expr_id` and AR1-V3's AL3 divergence;
- the `SysCabinRow` revision (D7, AR1-V4);
- cover-based serving.

**One AR1 §14 line overlaps AR.** AR's row lists "Waystone re-key"
(`ar1-architecture-revision-cabin-function.md:403`), and AP's D6 lists
"`waystone_root` re-keyed". AR1 §13 names one key change:
`waystone-concpets.md` §1/§3 moves to `(fetch_id, arg_hash)`. CLA reads it
as AP's, since a `fetch_id` that nothing keys on is dead code. On that
reading, AR's line is the chain shape's use of that key (§9.1). The reading
is AP-Q0's to confirm.

## 1. Survey at `4012617`

Read, not run. Every citation is to `4012617`.

### 1.1 The grammar has no scalar function and no expression

The manual states it as a choice. It says *"The grammar has no
expression tree at all — no arithmetic, no `OR`/`NOT` nesting, no function
calls outside the five aggregates. A smaller grammar every statement of
which executes predictably was chosen over a larger one"*
(`manual/sql/sql.md:907-911`). The AST agrees:

- **A WHERE conjunct** is one of:
  - a column compared with a literal or a column, including `IS [NOT] NULL`;
  - a subquery form, `IN`, `[NOT] EXISTS` or a scalar comparison;
  - a `BETWEEN` pair.

  See `PredicateKind` (`include/kds/parser/ast.hpp:197-224`) and
  `Condition` (`:229-263`).
- **A select item** is a column or one of the five aggregates
  (`SelectItem`, `:525-542`; `src/parser/parser.cpp:1346-1427`). AR1 §2
  keeps the aggregates out of the catalog.

**So AR1-1 and AR1-2 have no consumer in the tree.** A catalog with no entry
evaluates nothing. Every statement is D0, and D2's default protects nothing
until some function exists. AR1 names functions only as examples (`NOW()`,
`RANDOM()`, `RANDOM_SEED()`, `SUBSTR`, `DATE(ts)`) and never says which ones
ship. The first function is also the first expression in a predicate, and
that reverses the manual's stated choice. CLAUDE.md is plain about it:
*"do not add general-purpose DBMS features unasked"*. Which functions
exist, and in which position, is therefore the operator's (AP-Q1).

**The fingerprint does not move when functions land.** A function call
does not parse today, so no `sys.patterns` row holds its hash.
`fingerprint.hpp:161-166` makes "already fingerprintable" mean "already
storable", and making new statements storable needs no bump. Identifiers
are shape (`:39-44`), so `f(x) = 1` and `g(x) = 1` are two patterns with
no change to the accumulator.

### 1.2 `sys.patterns` is the trail directory table and nothing else

`SysPatternRow` (`include/kds/catalog/rows.hpp:493`) is keyed by
`pattern_id`. **Its one writer is the trail recorder**:

- `Catalog::RegisterPattern` is called from `src/stats/trail_recorder.cpp:57`
  alone.
- `TouchPattern` is called from `:156` alone.
- `ClaimPatternWaystoneRoot` is called from `:91` alone.

**Its statement-path readers are the replay lookup and the recorder's own
`FindPattern`** (`src/server/command_dispatcher.cpp:6526`,
`trail_recorder.cpp:42`). `stmt_class` is read only for display:
`SHOW PATTERNS` at `:1611` and the `sys.patterns` view at
`src/exec/catalog_view.cpp:153`.

The statistics identity AR1 §4 keeps on `pattern_id` is not in this table:

- the cabin optimizer's signals are in memory, keyed by `pattern_id`
  (`command_dispatcher.cpp:6864`, `src/stats/optimizer_signals.cpp:68`);
- `sys.access_stats` has its own key (`heap-and-tuple.md` §7).

**This changes D6's cost.** D6 adds `fetch_id` to the row and lets "several
pattern rows share one root". But no row exists except one the recorder
made in order to hold a root. So a row keyed by `fetch_id` is one root per
fetch by uniqueness of the key. That keying needs no layout change, and no
two rows have to agree on a root. The layout change D6 names has a real
price:

- The row is exactly 41 B (`rows.hpp:606`), and `CheckSize` refuses every
  other length as `Corruption` (`src/catalog/rows.cpp:10-15`).
- So a 49 B row needs a mechanism: a length-dispatching `Decode`, or a
  superblock move. AR1 supplied neither, which is the same gap AR1-V4
  found for D7.
- The superblock's precedent is AY-Q6 (C), which moved it 18 → 19 and
  refused every older volume (`include/kds/server/superblock.hpp:242`).

### 1.3 The instance key reaches the waystone page

The trail's instance key is `{pattern_id, arg_hash}`
(`command_dispatcher.cpp:6509`). The page header stores it
(`include/kds/stats/waystone.hpp:71`), and the walk compares it
(`src/stats/waystone_page.cpp:132`). Re-keying the trail on `fetch_id`
therefore changes what that header word, and the row's key, mean.

**Recording and replay are `SELECT`-only.**

- Replay is read in `HandleSelect` (`command_dispatcher.cpp:6494-6533`).
  `ANALYZE` is stripped before the parse, so it shares the statement's
  fingerprint.
- `RecordExecution` (`:6707`), which calls `RecordTrail`, has three
  callers, all `SELECT` paths: `RunAggregated` (`:6168`), `RunAnalyze`
  (`:6265`) and `HandleSelect` (`:6697`).

After the change, **every trail recorded before it misses**:

- every trail is a `SELECT`'s;
- a `SELECT`'s `fetch_id` never equals its `pattern_id`, because its
  select list is never empty;
- an equal hash is a 2^-64 collision, which also misses.

The miss costs the descents the trail would have saved and no result
(invariant 8). **The old rows are not like a retired version's, though.**

- `GetSysPatternRow` filters out rows of another fingerprint version
  (`src/catalog/catalog.cpp:2743-2758`).
- These rows carry the current version, so they stay visible to the
  lookup by hash and to `SHOW PATTERNS`, holding `pattern_id`s under
  whatever the key column is then called.

AP-Q2 has to say which mechanism hides them.

### 1.4 `fetch_id` needs one hash state and no second argument stream

AR1 §4 proposes two additions:

- a second argument stream that skips select-list literals;
- a BIND tag for a select-list `?`.

The select list holds neither (§1.1). So the argument half of the trail
key is `arg_hash` unchanged, and `fetch_id` needs one more FNV state in
`FingerprintAccumulator` (`include/kds/parser/fingerprint.hpp:273-307`).

The skip window starts after the **leading** `SELECT` and ends at the first
`FROM`. A subquery's select list stays shape because the subquery comes
after that `FROM`, outside the window. That is right, since
`IN (SELECT b FROM u)` and `IN (SELECT c FROM u)` fetch different values.

AR1 §4's "at parenthesis depth 0" matters only because `FROM` is not
reserved (`src/parser/lexer.cpp:38-43`). In `SELECT COUNT(from) FROM t`,
the `from` inside the call is a column, and a depth-blind window would end
there.

### 1.5 Sharing one trail across select lists is sound by construction

Two patterns of one fetch compile the same chain:

- The steps, access kinds, residuals and class of a statement do not read
  the projection (`src/exec/step_compiler.cpp:1898-1902`).
- An aggregated statement compiles to its unaggregated twin's chain (AG1,
  `:1915-1921`, pinned by the aggregate contract suite).

And replay accepts nothing it cannot verify:

- `Build` drops an entry whose `rel_oid` does not match its step, whose
  step is not replayable, or whose step is unknown
  (`src/exec/trail_replay.cpp:44-66`).
- `Find` is keyed on `(step_id, key derived from the frame)`
  (`src/exec/step_vm.cpp:572`, `:601`, `:634`).
- `VerifyTupleAt` re-checks the pk and the epoch, and visibility runs in
  `AcceptTupleAt`.
- Cabin hints, `BuildKey` and index probes never consult the trail, and
  `ORDER BY`/`LIMIT` can only cost misses.

A shared trail is therefore a valid location of that pk in that relation, or
a miss. This is AR1-V1's argument reaching one case further.

### 1.6 Rule 0′ still holds in the code, and the spec still says less

AR1-V1's reading re-verifies at `4012617`:

- `IsTrailReplayable` admits `kLookup` and `kProbe` only
  (`include/kds/exec/step_chain.hpp:164`).
- Both replay call sites pass the key derived from the frame (§1.5).

`waystone-concpets.md:39` still scopes rule 0 to *"any join replay"*. D8's
wording change is unbuilt. **No SQL can reach the driving-step case today**:
without functions, one instance key means one literal and so one driving
key. `tests/waystone_contract_test.cpp:317-371` plants a trail with the pk
kept and the slot moved, which is rule 1, not rule 0.

### 1.7 A predicate conjunct is read as `col op rhs` in a dozen places

`src/exec/step_compiler.cpp` reads a `StepPredicate`'s column and operand
directly for each of these:

| reader | lines |
|---|---|
| the pk bounds | `:516-562` |
| index literal bounds | `:580-610` |
| the Cabin probe pick | `:764-832` |
| join keys | `:861-877` |
| `BuildKey` | `:902-939` |
| the read-column masks | `:1084-1172` |

Suppose `DATE(ts) >= '2026-10-01'` were lowered as a conjunct on `ts`. Then
any of these could make it a key, a bound or a probe on `ts`, and the
statement would return wrong rows with no error. **This is the one
quiet-wrong surface AP itself opens.**

## 2. Rulings — CLA's proposals, unmarked

**AP-R1 — `fetch_id` is one more FNV state, fed only for a `SELECT`.**

- **Rule.** After a leading `SELECT`, the second state folds every token
  `pattern_id` folds, except those after that `SELECT` and before the
  first `FROM` at parenthesis depth 0 (§1.4). For any other leading word
  the state is not fed, and `Result()` returns `shape_` as `fetch_id`. A
  write therefore pays nothing, and `fetch_id == pattern_id` holds for it
  by construction.
- **Rides with the parse.** It runs on the same `Feed`, so nothing lexes
  twice. `FingerprintOf` returns it too, so there is one set of rules
  (`fingerprint.hpp:246-258`).
- **`Fingerprint` gains `fetch_id`.** `pattern_id`, `arg_hash`,
  `literal_count` and `param_count` are unchanged.
- **One version covers both hashes.** From this stage on, a change to
  `fetch_id`'s skip rule is a bump under the rule at `:144-171`, and that
  file's comment says so.
- **The golden corpus** pins `fetch_id` beside every `pattern_id` it
  already pins. Those `pattern_id` values must not move.

**AP-R2 — the trail keys on `{fetch_id, arg_hash}` and statistics keep
`pattern_id`.** Its row and display halves follow AP-Q2's mark.

- **Moves to `fetch_id`:**
  - the replay lookup (`command_dispatcher.cpp:6509-6526`);
  - `RecordTrail`;
  - the recorder's find/register/touch/claim calls;
  - the waystone page header's key word.
- **Keeps `pattern_id`:** the optimizer's `NoteExecution` (`:6864`) and
  `sys.access_stats`.
- **`ANALYZE` prints both.** Its `pattern_id=` (`:6289-6298`) exists so
  that a statement can be matched to the row `SHOW PATTERNS` lists. Once
  the row is keyed by `fetch_id`, that match needs `fetch_id=` beside it.
- **The wire is AP-Q4's.** `S_PARSE_OK` returns `pattern_id`
  (`src/server/kwp_session.cpp:506-522`, `docs/spec/protocol.md:79`), and
  the matching argument applies to it too.

**AP-R3 — rule 0′ as wording** (D8 as AR1-V1 amends it).

- **`waystone-concpets.md` rule 0.** "Mandatory before any join replay"
  becomes "mandatory before any replay, the driving step included". The
  mechanism is stated as the index key `(step_id, pk)`, not as a check.
- **`parser-v2.md` I17** gets the same scope sentence.
- **No engine change.**
- **A new cell.** It plants a trail through `stats::WriteTrail`, as
  `waystone_contract_test.cpp:317-371` does, with the driving step's entry
  under a pk other than the one the statement derives. The cell shows the
  entry is never found. No existing cell covers this (§1.6).

**AP-R4 — the function catalog, built with its first functions and not
before them** (AP-Q1). The catalog, purity and the determinism class land
in the same stage as the first entries that use them, in this order:

1. **The catalog.**
   - Entries are compiled into the binary. Each declares its purity, and
     an entry that declares none is `kVolatileRow` (D2): the struct's
     default value, which a test pins.
   - A test build can add entries through a `RegisterFunctionForTest`
     seam, after the repository's `…ForTest` hooks
     (`include/kds/server/command_dispatcher.hpp:646`). That seam is how
     the D2 cells get a `kVolatileRow` entry without one shipping.
2. **The predicate side.** `[quiet-wrong]`, §1.7.
   - The first expression the grammar admits is a function call on the
     column side of a WHERE conjunct, compared to a literal or `?`:
     `F(col, …) op value`.
   - It lowers to **a residual kind of its own**, a function conjunct,
     which none of §1.7's readers walks. It is filtered after the step's
     access, never used as a step key, a bound, an index key, a join key, a
     `BuildKey` or a Cabin probe. The read-column mask still includes its
     input columns.
   - Serving a function predicate from a structure is AQ's cover.
   - Its NULL handling is `null.md` §4's tri-state, collapsed at the one
     site.
3. **The determinism class** is computed at parse, over predicate
   position only (AR1 §3):
   - **D0:** only `kImmutable` functions, or none.
   - **D1:** at least one `kStable` or `kVolatileStatement` function.
     Whether the fold into the instance key is built in AP is AP-Q5.
   - **D2:** at least one `kVolatileRow` function. The statement is
     neither registered, recorded nor observed. Statistics still count its
     `pattern_id`. "Not observed" means no sighting
     (`TrailRecorder::Observe`) and no Cabin observation. The second is
     already true, since Cabin recording evaluates only the cabin's own
     conjunct (`src/exec/step_vm.cpp:2195-2210`), and the stage cites
     that rather than adding a gate.
4. **Refusals.**
   - A function in the select list, `ORDER BY` or `GROUP BY` is refused
     `NotImplemented` at its position, and so is a nested call.
   - A name not in the catalog is refused per AP-Q3.
   - An aggregate's name in predicate position keeps the refusal it has
     today, and is never reported as an unknown function.

**What D2 protects inside AP is cost, not answers.** A D2 statement
recorded by mistake would replay through rules 0, 0′ and 1, and so miss.
D2 becomes a quiet-wrong guard when AQ evaluates `F` on the write path
(AR1 §10). AP builds it now because AR1 orders it now, and so that AQ
finds it in place.

## 3. Stages

Each stage waits for the operator's word.

| stage | what | exit | size |
|---|---|---|---|
| AP-S0 | This order; the index row | the files at the commit | S |
| AP-S1 | **`fetch_id`** (AP-R1) | <ul><li>Unit cells: two select lists over one `FROM … WHERE` converge.</li><li>Two subquery select lists do not.</li><li>An aggregated and a plain select list converge.</li><li>`SELECT COUNT(from) FROM t` and `SELECT COUNT(x) FROM t` converge.</li><li>Every non-`SELECT` leading word gives `fetch_id == pattern_id`, including BI5's multi-row `INSERT`.</li><li>The golden corpus with `fetch_id` added and every `pattern_id` unchanged.</li><li>**Mutation:** the window reopened after every `SELECT`, killed by the subquery cell; depth ignored, killed by the `COUNT(from)` cell.</li></ul> | S |
| AP-S2 | **The trail on `fetch_id`** (AP-R2) - **gated on AP-Q2's mark** | <ul><li>Red first: `SELECT a …` records and `SELECT b …` with the same arguments replays nothing today.</li><li>Green: it replays, and its result is byte-identical with replay off (the waystone contract suite, extended).</li><li>An aggregated and a plain statement over one fetch, both orders, results identical to replay off.</li><li>A pre-change row is never found and never displayed as a `fetch_id` (AP-Q2's mechanism), and the statement records again.</li><li>The optimizer's signals still split by `pattern_id`.</li><li>**Mutation:** the lookup back on `pattern_id`, killed by the red cell.</li><li>`waystone-concpets.md` §1/§3/§5, the `sys.patterns` view, `SHOW PATTERNS`, `protocol.md` per AP-Q4, and `manual/` restated.</li></ul> | M |
| AP-S3 | **Rule 0′** (AP-R3) | <ul><li>The two spec sentences.</li><li>The planted driving-step cell.</li><li>No `src/` change.</li></ul> | S |
| AP-S4 | **The function catalog and the determinism class** (AP-R4) - **gated on AP-Q1** (and AP-Q5 for D1) | <ul><li>Per function: its evaluation, its NULL and type refusals, each with a position.</li><li>**§1.7's readers, `[quiet-wrong]`:** a function conjunct over the pk, over an indexed column, over a cabined column, over a join column, and over a column a `BETWEEN` would bound. Each result is byte-identical to a scan oracle with replay, index and Cabin off.</li><li>A D0 statement with an immutable function and a pk equality beside it (a keyed step, so it can record) records and replays.</li><li>A D2 statement through the test seam is never sighted, registered or recorded.</li><li>The default-purity cell.</li><li>D1 per AP-Q5.</li><li>The refusals of AP-R4 item 4, including `WHERE SUM(x) = 1` keeping its present refusal.</li><li>**Mutation:** the function conjunct lowered as a plain `StepPredicate` on its column, killed by the reader cells; the D2 gate removed, killed by the D2 cell; the default purity flipped to `kImmutable`, killed by the default-purity cell.</li><li>`parser-v2.md`, `fingerprint.hpp`'s comment on functions, `manual/sql/sql.md:907-911` rewritten, and `types.md` per function.</li></ul> | L |
| AP-S5 | **AP's close** | <ul><li>A row per stage.</li><li>What AP carries.</li><li>The overhead measured once over the whole change. A second FNV state runs on every token of every `SELECT`, so the cell is the parse-bound point `SELECT`.</li></ul> | S |

## 4. Items for the operator

| # | item | class | CLA proposal |
|---|---|---|---|
| AP-Q0 | **The letter**: whether this order opens as AP, and whether the trail's re-key to `fetch_id` is AP's rather than AR's "Waystone re-key" (§0) | scope | yes to both. It is AR1 §14's first order, and AZ-Q5 settled it first. A `fetch_id` with nothing keyed on it is dead code |
| AP-Q1 | **The first functions, and where they may appear**. The grammar has none, and the manual states that as a choice (§1.1). Without a function, AP-S4 builds a catalog nothing calls | scope, user-visible | AP-S1..S3 proceed without it, and AP-S4 waits for this mark. Two candidates, each an example AR1 itself gives: `DATE(timestamp) → date`, `kImmutable` with a cover (§9.2's example, and the first thing AQ's expression shape needs); and `NOW()`, `kStable` (§2's example, the D1 path). **No `kVolatileRow` function ships**, since a row-random predicate has no OLTP use; D2 is exercised through the test seam. Position: the column side of a WHERE conjunct only (AP-R4) |
| AP-Q2 | **D6's shape, and how the old rows go**. Every shape below amends a ratified sentence, which is named in each case | format | **(b)**. It retires the old rows with the one mechanism built for exactly this, and it costs nothing (a) and (c) do not also cost, since every stored trail misses under all three (§1.3). See the options below |
| AP-Q3 | **A function name not in the catalog**: `InvalidArgument` or `NotImplemented` (a wire surface, `protocol.md` §11) | user-visible | `InvalidArgument`, as an unknown column is. The parser cannot tell a function KDS lacks from a misspelling, and `NotImplemented` would claim the first |
| AP-Q4 | **The wire's `pattern_id`** in `S_PARSE_OK` (§AP-R2) | user-visible, wire | keep it as the statement's `pattern_id`, which stays true, and say in `protocol.md` that `SHOW PATTERNS` keys by `fetch_id`. Adding `fetch_id` to the frame is a wire change with no client asking for it |
| AP-Q5 | **D1's fold into the instance key, now or with AQ** | cost | **with AQ**. Inside AP it buys nothing: replay is sound without it (AR1-V1). Built now it costs something. A per-execution instance is sighted once and never reaches the n=2 threshold (`src/stats/trail_recorder.cpp:8-18`). Every `NOW()` execution also adds a sighting entry, and at `kMaxSightings` (4096, `include/kds/stats/trail_recorder.hpp:69`) the table clears wholesale (`trail_recorder.cpp:21-29`), so one hot dashboard query would starve recording for every pattern. It also has no seam: KWP's BIND is text substitution (`protocol.md:84`), and the instance key is formed from the parse at `command_dispatcher.cpp:6508-6510`. Until AQ, a D1 statement is treated as D0 by every AP consumer. This defers part of ratified AR1-2 |

**AP-Q2's options.** Every stored trail misses under all three (§1.3).

- **(a) Key the existing row by `fetch_id` and mark it with a bit in
  `flags`.**
  - The `u16` has no writer and no reader (`rows.hpp:554-559`).
  - `GetSysPatternRow` filters on the bit, and the displays label an
    unmarked row as a `pattern_id` row.
  - No layout change, and no version bump.
  - Amends D6's "`SysPatternRow` gains `fetch_id`" and the `rows.hpp`
    comment on the key field.
- **(b) Key the existing row by `fetch_id` and bump `kFingerprintVersion`
  to 2.**
  - `GetSysPatternRow`'s existing filter (`catalog.cpp:2743-2758`) hides
    every old row by construction.
  - The displays already show `fingerprint_version`.
  - Amends D6, and AR1-3's "`kFingerprintVersion` does not move". The
    `pattern_id` values do not move. The bump says what the row's key
    hash now is, and the rule at `fingerprint.hpp:146-158` gains that
    case.
- **(c) D6 as ratified.**
  - A 49 B row and a format mechanism: a length-dispatching `Decode`, or
    the superblock 19 → 20 refusing every older volume (AY-Q6 (C)'s
    precedent).
  - Plus a rule for which of a fetch's several rows holds its root.
  - AP-S2's exit then gains the format cells. A refused older volume is
    the same cost AY-S8 paid.

AR1's ratified D2 and D8 are both `[quiet-wrong]`. AP builds them as
ratified (AP-R4, AP-R3) and re-decides neither. AP-R4 item 2 is the one
`[quiet-wrong]` surface AP opens itself. Its shape is a ruling here, not
an open item, and AP-S4's cells are its exit.

## 5. Sequencing

- **AP-S1 before AP-S2.** S2 keys on the hash S1 produces, and it also
  waits for AP-Q2.
- **AP-S3 is independent.** It is docs and one cell.
- **AP-S4 after AP-S1.** Both change the accumulator and the parse, and
  S4 waits for AP-Q1 besides.
- **AP-S5 is last.** The milestone's overhead is measured once over its
  whole code change (`raft-marks-2026-09-30.md` §8), from `4012617` to the
  commit that closes it. If AP-Q1 is still unmarked when S1-S3 have
  landed, CLA proposes that AP-S5 close over them and carry AP-S4. AQ then
  opens with AP-S4 as its first stage, since AQ's expression shape needs a
  `kImmutable` function anyway.

## 6. Row status

### AP-S0 — written 2026-10-02

On `worktree-ap-s0-order` from `4012617`, as this file and its index row.
§1's survey was read against `4012617` and not run. No code, spec or test
is changed. The letter itself is AP-Q0's.

The operator's word is recorded here. Today's `raft-marks-2026-10-02.md`
is held untracked by another session's worktree,
`az-q3-release-any-failed-check`.

**The review** (`critics-developer`, on `2905ec6`) confirmed by source
read the claims that replay is sound, and made §1.5 stronger: the chain
does not depend on the select list (AG1), so the hedge went. Its
correctness findings were all applied:

- **`[quiet-wrong]`, the most serious.** AP-R4 said "never a step key"
  but gave no representation for it. §1.7 and a function-conjunct
  residual kind were added, with a cell per reader and a mutant.
- **Gating.** AP-S2 is now gated on AP-Q2.
- **AP-Q2.** It now names each ratified sentence it amends and offers
  three options. §1.3's "like a retired version" was wrong: the old rows
  carry the current version and stay visible.
- **New operator items.**
  - The overlap with AR's "Waystone re-key" goes to AP-Q0.
  - The wire's `pattern_id` is AP-Q4.
  - D1's fold is AP-Q5. Its cell could not be met, and the fold would
    starve the sighting table.
- **Two corrections to AP-R4.** D2's "nor observed" was restored, with
  the reason the Cabin half needs no gate. `kVolatileStatement` was put
  back in D1.
- **Cells.**
  - AP-S1's mutation cell could not kill its mutant: depth is not what
    keeps a subquery out of the window. It was replaced by the
    leading-`SELECT` mutant and a `COUNT(from)` cell.
  - Rule 0′'s cell must plant its trail, and no existing cell covers
    it.
  - AP-S4's record cell needs a keyed step.
  - The D2 entry needs a named test seam.
  - An aggregate's name in a predicate keeps its present refusal.
- **Citations.** `rows.hpp:606`, the `ast.hpp` ranges, the recorder's
  own `FindPattern` as a second reader, and the M4 citation were
  corrected.

The trims were taken: the `stmt_class` bullet, the select-list hedge,
the doubled `fetch_id == pattern_id` sentence, and §6's paragraph on the
other session. AP-R1 now feeds the second state only for a `SELECT`, so a
write pays nothing.

**Rejected: none.**
