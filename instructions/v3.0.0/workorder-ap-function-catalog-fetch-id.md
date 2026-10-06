# Work order AP — `fetch_id`, rule 0′ and the function catalog

Written 2026-10-02 on `worktree-ap-s0-order` from `4012617`
(`v2.7.0-573-g4012617`). The operator's word was *"b"*, answering CLA's
three options with *"Write AP's work order (AP-S0) from AR1 §14"*. AR1 is
ratified (`raft-marks-2026-09-29.md` §5), and AZ-Q5 settled AP first
(`raft-marks-2026-09-30.md` §16). AR1's status line says that settlement
*"opens no letter"*; the word above is what opens this one.

**Opened as AP on 2026-10-02, with every §4 item marked as proposed**
(`raft-marks-2026-10-02.md` §4). Every stage waits for its own word. It cuts no tag; the v3.0.0 tag waits on M4
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

`FROM` is not reserved (`src/parser/lexer.cpp:38-43`), so a column may be
named `from`, and AR1 §4's "first `FROM` at parenthesis depth 0" is not
enough (AP-S1's review):

- Inside a call, `SELECT COUNT(from) FROM t`, the depth passes over it.
- At depth 0, `SELECT from FROM t`, `SELECT a, from FROM t` and
  `SELECT t.from FROM t` all parse.

The select list is `*` or items, and an item is a column, `rel.column`, or
an aggregate over one (`src/parser/parser.cpp:1346-1427`). So a
column-named `from` at depth 0 begins an item: it comes right after the
`SELECT`, a `,` or a `.`. The `from` that ends the list follows the end of
an item.

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

## 2. Rulings — CLA's proposals, marked as proposed 2026-10-02

**AP-R1 — `fetch_id` is one more FNV state, fed only for a `SELECT`.**

- **Rule.** After a leading `SELECT`, the second state folds every token
  `pattern_id` folds, except those after that `SELECT` and before the
  `FROM` that ends its select list. That `FROM` is the first `from` at
  parenthesis depth 0 that does not begin an item (§1.4); the original
  rule, amended at AP-S1's review, said only depth 0. For any other leading word
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
   - The first expression the grammar admits is a function call on
     either side of a WHERE comparison: `F(col, …) op value`,
     `col op F(…)`, or one on each side. The value side was added by
     the mark of 2026-10-02 (`raft-marks-2026-10-02.md` §5), because
     `NOW()` takes no column.
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
| AP-S1 | **`fetch_id`** (AP-R1) | <ul><li>Unit cells: two select lists over one `FROM … WHERE` converge.</li><li>Two subquery select lists do not.</li><li>An aggregated and a plain select list converge.</li><li>`SELECT COUNT(from) FROM t` and `SELECT COUNT(x) FROM t` converge, and so do `SELECT from`, `a, from` and `t.from` with `SELECT a` (added at the review).</li><li>Every non-`SELECT` leading word gives `fetch_id == pattern_id`, including BI5's multi-row `INSERT`.</li><li>The golden corpus with `fetch_id` added and every `pattern_id` unchanged.</li><li>**Mutation:** the window reopened after every `SELECT`, killed by the subquery cell; depth ignored, killed by the `COUNT(from)` cell; the item-begins guard removed, killed by the column-named-`from` cell.</li></ul> | S |
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
  commit that closes it.

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

### AP opened, and §4 marked - 2026-10-02

On *"CLA 제안대로 진행하고 main에 push해줘"*, every §4 item was marked as
proposed (`raft-marks-2026-10-02.md` §4), and the order landed on `main`.

- **Open, each on its own word:** AP-S1, S2, S3 and S5.
- **AP-S4** is open too, since AP-Q1 named its functions: `DATE(timestamp)`
  and `NOW()`.
- **AP-S2 takes AP-Q2 (b).** The row is keyed by `fetch_id`, and
  `kFingerprintVersion` moves 1 → 2. AP-S2's exit therefore gains three
  things:
  - the bump;
  - the golden corpus re-pinned at version 2, with every `pattern_id`
    value unchanged;
  - a cell showing that a version-1 row is never found.
- **AP-S4 builds no D1 fold** (AP-Q5). A `NOW()` statement is D0 to every
  AP consumer.

No stage has started.

### AP-S1 — built 2026-10-02

On `worktree-ap-s1-fetch-id` from `f992e7e`, on *"AP-S1 시작해줘"*.

- **Built.** `Fingerprint::fetch_id`, from one more FNV state in
  `FingerprintAccumulator`. It is fed only after a leading `SELECT`, so
  every other leading word returns its `pattern_id` as its `fetch_id`.
  The shape fold became one helper applied to both states, which is what
  guarantees the two hashes differ only by the window's tokens.
- **Nothing persisted moves.** `pattern_id`, `arg_hash` and
  `kFingerprintVersion` are byte-unchanged: the corpus gained a
  `fetch_id` column, and its other columns regenerate identically on all
  287 rows. Two rows moved within the new column at the review, both
  refused `InvalidArgument` and never storable (`SELECT FROM t`,
  `SELECT a, FROM t`).
- **The pin** `0xb5b2ac05aab0a3a9` was computed outside the code, by an
  FNV model of the shape rules that reproduces `pattern_id`'s own pin.
- **Mutants.** Each was run against the built tree, and each was killed:
  - depth ignored, killed by the `COUNT(from)` cell;
  - the window reopened at every `SELECT`, killed by the subquery cell
    and the corpus;
  - the item-begins guard removed, killed by the column-named-`from`
    cell and the corpus.
- **Suite.** 3083/3083 at `6f32eab`, and after the review 3084/3084, both
  under `ctest -LE heap-suspended -j8`, Debug. Overhead not measured; it
  is measured at the milestone's close (AP-S5).

**The review** (`critics-developer`, on `6f32eab`) checked the change's
claims by source read: the persisted bytes, the corpus, both pins, and
every window case it listed. It found one defect, which was applied.

- **The defect.** A column named `from` at depth 0 closed the window
  early, so one fetch got two keys: a miss, never a wrong row. AP-R1 and
  §1.4 are amended above. The window now closes only on a `from` that does
  not begin an item.
- **Applied:** the misplaced allow-list comment moved back, and the
  `kKeyword` arm dropped from the `from` test, since `FROM` is not a
  keyword.
- **Rejected:** deleting `FingerprintAccumulator::Reset()`, which no code
  calls. It is a public method that predates this stage, and removing it
  is outside AP-S1.
- **Rejected:** making `FingerprintOf` drain its lexer's own accumulator
  instead of feeding a second one. The double hash predates the stage,
  and its one caller is the golden corpus, not the statement path.


### AP-S2 — built 2026-10-02

On `worktree-ap-s2-trail-on-fetch-id` from `d3d90b5`, on *"main에 push하고 AP-S2 시작해줘"*.

- **Built.**
  - The trail keys on `{fetch_id, arg_hash}`, so statements differing
    only in their select list share one trail and one `sys.patterns` row.
  - The dispatcher carries `StatementIdentity` `{trail, pattern_id}`.
    The cabin optimizer still counts by `pattern_id`, and `ANALYZE`
    prints both ids.
  - The trail layer's key field is `fetch_id` throughout: `InstanceKey`,
    the waystone page header, `SysPatternRow`, `PatternAccess`, the
    catalog calls, the `sys.patterns` view and `SHOW PATTERNS`. The bytes
    and the layout are unchanged.
- **The version.** `kFingerprintVersion` is 2, per AP-Q2 (b), and no hash
  moved. A version-1 row is hidden by the existing filter, and
  `SHOW PATTERNS` lists it `stale=v1` under the `pattern_id=` it was
  keyed by.
- **The wire.** `S_PARSE_OK`'s `pattern_id` is unchanged, per AP-Q4, and
  `protocol.md` says so.
- **Cells.**
  - Another select list replays the trail, on the btree and the heap
    relation.
  - Aggregated and plain statements share a trail in both orders, and the
    reply is unchanged.
  - `ANALYZE` prints both ids.
  - A version-1 row planted under the statement's own `fetch_id` is never
    found, is labelled `pattern_id=`, and the statement records again.
  - The optimizer still keeps two select lists apart.
  - The contract suite gained six shared-fetch statements across all five
    configurations.
- **Mutants.**
  - The key back on `pattern_id`, which is the pre-AP-S2 behaviour and so
    the red-first check, was killed by the three replay cells.
  - The optimizer counting by `fetch_id` was killed by its own cell and
    the `ANALYZE` cell.
- **A cell's premise changed.**
  `WaystoneAcrossCores.APeersRepeatedStatementRegistersItsPattern` relied
  on a different select list making a second row. Its peer now runs a
  different fetch (`LIMIT 1`).
- **Suite.**
  - 3089/3089 at `749af9b`.
  - After the review and the merge of `origin/main` (`17f4e1b`): 3090/3090
    of 3091 registered, one disabled as before. The first run of that
    tree failed `IdAllocationAcrossCores.TwoCoresWritingOneRelationIssueOneSequence`
    at its 20 s rig timeout under `-j8`. The cell is on the insert path,
    which this stage does not touch. It then passed 10/10 alone and in a
    clean full run.
  - All runs used `ctest -LE heap-suspended -j8`, Debug.
  - Overhead not measured; it is measured at AP-S5.

**The review** (`critics-developer`, on `749af9b`) found no engine defect.
It confirmed by source read:

- **Sharing is sound.** The steps, the class and the step numbering do
  not read the projection. `ReadColumnsOf` always decodes the pk of a
  replayable step.
- **The rename is complete in code**, and the version bump is correct
  for every consumer.

Applied:

- **A test defect, fixed by the reviewer.** `ACorruptedTrailChangesNoReply`
  iterated queries, not instances, so a shared instance could be poisoned
  twice and restored. It now poisons each instance once.
- **Stale sentences** that still said `pattern_id` where they meant the
  trail key: `RunAnalyze`'s header, the `SHOW PATTERNS` doc line,
  `trail_replay.hpp`, `step_compiler.hpp`, `parser-v2.md` (the chain-layout
  sentence, I7, J5), `heap-and-tuple.md`, `step_correlation_test.cpp`, and
  `fingerprint.cpp`'s "stays 1".
- **The version-1 label**, so a pattern_id is never printed as a
  `fetch_id=`.

Rejected:

- **Collapsing the seven version pins to one.** The `TypesContract` and
  `AggregateContract` pins are numbered spec items in suites that keep one
  test per item (CLAUDE.md). The four in `fingerprint_test.cpp` each pin
  the hash beside the version for a named past change.
- **The unreachable `!identity` guard in `RecordTrail`.** It predates the
  stage.
- **A `MutatePatternRow` version-filter cell.** The gap predates the stage
  and belongs in `catalog_test.cpp`. It is recorded here, not taken.

### AP-S3 — built 2026-10-02

On `worktree-ap-s3-rule-0-prime` from `7fc2c57`, on *"main에 push하고 AP-S3 시작해줘"*.

- **The wording.** `waystone-concpets.md` §2 rule 0 now covers every
  replayed step, the driving step included, where it said "any join
  replay". `parser-v2.md` I17 and its `Lookup` row say the same.
- **No code change.** The code already met the rule. Two comments were
  corrected (`trail_replay.hpp`, `step_vm.cpp`), so the exit's "no `src/`
  change" holds for code and not for comments.
- **What mutation found.** The rule is held twice, and the spec now says
  so:
  - the replay index finds an entry only by the key the step derived;
  - `TryReplay` verifies the location against that same derived key,
    since a `TrailLocation` carries no pk.
  - So rule 1's sentence now names the derived key. **This goes beyond
    AP-R3's wording**, which named the index key alone. The second hold is
    a fact of the code that mutation surfaced, not ratified text.
- **The cell.** `ADrivingEntryForAnotherKeyIsNeverFoundEvenWhereItIsValid`
  plants, under row 3's instance key, an entry for row 5 at row 5's real
  location and epoch. It asserts the reply, and that the entry is neither
  served nor found.
- **Mutants.**
  - `Find` ignoring its key: killed by this cell.
  - The verifier's pk check removed: killed by
    `AWrongKeystoneAtTheTargetIsAMissNotAWrongRow`.
  - Both together: killed by this cell.
- **Suite.**
  - 3091/3091 at `0fcdfcc`.
  - After the review, the first full run failed
    `IdAllocationAcrossCores.TwoCoresWritingOneRelationIssueOneSequence` at
    its 20 s timeout, as in AP-S2. The rerun was 3091/3091.
  - The flake is now recorded in `known-gaps.md`, Testing.
  - All runs used `ctest -LE heap-suspended -j8`, Debug. Overhead not
    measured; it is measured at AP-S5.

**The review** (`critics-developer`, on `0fcdfcc`, read-only) confirmed
every claim the commit made about `src/`. It found no code defect. Applied:

- **The cell claimed more than it asserted.** It showed "never served",
  not "never found". It now also asserts no `trail_misses=`, which is what
  kills the `Find` mutant that had survived.
- **`EntryOf` copies the entry's `page_epoch`.** It planted epoch 0, which
  matched current pages only by coincidence.
- **Rule 1 contradicted the new rule 0.** It said "the entry's `pk`"; it
  now names the derived key.
- **Provenance and history** were removed from the rule's text, and
  "driving row" became "outer row", so it no longer collides with "driving
  step".
- **I17's mechanism sentence** now cites rule 0 instead of restating it.
- **Test comments.** The `arg_hash`-collision caveat is added, the
  re-plant's reason corrected, and the comment trimmed to the spec
  citation and the mutants it kills.

**Rejected: none.**

### AP-S4 — built 2026-10-02

On `worktree-ap-s4-function-catalog` from `db5a5e6`, on *"main에 push하고
AP-S4 시작해줘"*.

**The mark taken on the way.** AP-Q1 as first marked put a call on the
column side only, and `NOW()` takes no column, so it could filter no row.
Asked, the operator answered with the value side too; that is
`raft-marks-2026-10-02.md` §5, and AP-R4 is amended above.

**Built.**

- **The functions.** `DATE(timestamp)` (`kImmutable`, floor division) and
  `NOW()` (`kStable`), on either side of a WHERE comparison, including
  `IS [NOT] NULL`.
- **The AST.** A call parses to `PredicateKind::kCompareFunction`, which
  every raw-condition reader that acts on a `kCompareValue` passes by.
- **The compiled form.** It lowers to `Step::fn_residual`, a residual kind
  that none of §1.7's readers walks.
  - It is evaluated at the two sites every accepted row passes.
  - Its columns are in both decode masks and both correlation walkers.
  - A step carrying one takes no build.
- **The catalog** (`include/kds/exec/functions.hpp`).
  - Purity is declared per entry, and `kVolatileRow` is the struct's
    default.
  - The determinism class is folded at compile over every sub-chain.
  - A D2 statement keeps its statistics identity, and takes and reads no
    trail. D1 is D0 (AP-Q5).
  - The test seam is `ScopedTestFunction`, where AP-R4 named it
    `RegisterFunctionForTest`. A statement clock pin,
    `ScopedStatementClockForTest`, sits beside it.
- **`NOW()`** is taken once per statement. A write parked mid-walk carries
  its instant in `Session::ParkedWrite`, so the resume, which compiles
  again, filters against the same one.
- **Refusals.**
  - An unknown name is `InvalidArgument` (AP-Q3).
  - These are `NotImplemented`, each at its byte: a call outside a WHERE
    comparison (select list, `ORDER BY`, `GROUP BY`, `HAVING`, a catalog
    view, any value position), a nested call, a literal argument, and
    `IN`/`BETWEEN` against a call.
  - An aggregate's name keeps its old refusal.
- **Unchanged.** The golden corpus did not move.

**Cells: 19 in `tests/function_conjunct_test.cpp`, and one in
`tests/lock_family_test.cpp`.**

- **The reader group.** Through a test-only `plus_one(int64)`: the pk, an
  indexed column, a Cabin that serves (`cabin_hits=`), a join and a walked
  join, `BETWEEN`, a correlated sub-chain reached only through a function
  argument, and a sub-chain placed at a later step only through one.
- **Mutants, each killed.**
  - Lowered as a plain `StepPredicate`: killed by every reader cell.
  - The D2 gate removed.
  - The default purity flipped.
  - `ReferencesAnOuterChain` blind to function columns.
  - `DeepestReferenceIntoThisChain` blind to them.
  - The build decline removed.
  - The resume taking a fresh `NOW()`: killed by
    `MidWalkWaitTest.NowIsOneInstantAcrossAMidWalkPark`.

**Suite.**

- 3108/3108 at `010b8e0`. That commit went to `main` at the operator's
  word before its review returned, through the pre-push hook.
- After the review, 3111/3111.
- Both runs used `ctest -LE heap-suspended -j8`, Debug.
- Overhead is measured at AP-S5.

**The review** (`critics-developer`, on `010b8e0`, read-only) confirmed by
source read that a function conjunct is never a key and is evaluated on
every accepted row, on every path it listed. Applied:

- **B1 - `NOW()` re-taken when a parked write resumes.** This was a defect
  of this stage: the resume compiles again. The instant is now carried in
  the park, with the cell above, red before the fix.
- **B2.** Two sub-chain switches lacked the new kind, and one fell through
  to continue. Both arms were added.
- **Simplifications.**
  - The comparison's right side parses in one place.
  - The literal arm of the lowering coerces against the call's result type
    only, the one way it is reached. Its `len` limit is stated.
  - Arguments are passed by pointer, so a filtered row copies no value.
- **Test gaps.**
  - The Cabin cell's fixture had no Cabin store, so the Cabin never served.
    It does now, and the cell asserts the hit.
  - Every function conjunct of a compile shares one instant.
  - The placement walker has its cell.
  - Two refusal positions are pinned.
- **Docs.**
  - I10 now names the value positions, and those positions now refuse a
    call by name at its byte, not as a malformed value. The catch-all
    "expected value" carries its byte as well, which it did not before.
  - The `CompileWhere` header names `fn_residual`.

**Recorded, not applied:**

- **B3, a pre-existing defect.** A nested uncorrelated subquery is dropped
  and the statement answers as if it were absent. Confirmed by running it.
  It is `docs/inflight/bugs/a-nested-uncorrelated-subquery-is-dropped.md`
  and is outside AP.
- **The cabin optimizer's candidate pick.** `RecordOptimizerSignals` takes
  the lowest filtered column as a step's Cabin candidate, and that can now
  be a function's argument. This costs, and does not answer wrongly.

### AP-S5 — the close, 2026-10-02

On `worktree-ap-s5-close`, which merged AP-S4's branch over `origin/main`
at `e7617b2` (BA-S0, documents only), giving `0c3268f`.

| stage | what landed | commits | suite |
|---|---|---|---|
| AP-S0 | the order; opened with AP-Q0..Q5 marked as proposed | `2905ec6`, `8a1c017`, `f992e7e` | not executed (documents) |
| AP-S1 | `fetch_id` | `6f32eab`, `5971270`, `d3d90b5` | 3083/3083 at `6f32eab`; 3084/3084 at `5971270` |
| AP-S2 | the trail and `sys.patterns` on `fetch_id`; version 1 → 2 | `749af9b`, `7fc2c57` | 3089/3089 at `749af9b`; at `7fc2c57` the first run failed `IdAllocationAcrossCores` at its `-j8` timeout, the rerun 3090/3090 |
| AP-S3 | rule 0′ as wording, and the planted cell | `0fcdfcc`, `db5a5e6` | 3091/3091 at `0fcdfcc`; at `db5a5e6` the same first-run timeout, the rerun 3091/3091 |
| AP-S4 | the function catalog, `DATE` and `NOW()`, the function conjunct | `010b8e0`, `c4b82e4` | 3108/3108 at `010b8e0`; 3111/3111 at `c4b82e4` |

Every count is the stage row's own, run by CLA in this session with
`ctest -LE heap-suspended -j8`, Debug; none was re-run at the close.

**The overhead, measured once over the whole change** (`ck-tester`,
`bench/v3.0.0/results-ap-s5-overhead-v2.7.0-606-g0c3268f.md`, committed at
`795c9f0`). A = `4012617`, B = `c4b82e4`, Release built from `git archive`,
`cores = 1`, `relaxed`, BTREE, interleaved.

- **The range is not AP alone.** `4012617..c4b82e4` also carries AZ-R5's
  engine commits `7ce9718`, `878f40a`, `e2340c4` and `9439497`. Their path
  is foreign-key only, and no cell reaches it.
- **No resolvable cost** in these cells: the point `SELECT` (narrow or
  wide select list), the pk `UPDATE`, and the walking `UPDATE`.
- **One per-row cost** on a filter scan that rejects nearly every row:
  - +1.35 / +4.65 / +79.2 µs at 1K / 10K / 60K rows, which is +1.0 /
    +0.6 / +1.9 %, about 0.5–1.4 ns per examined row;
  - 9, 8 and 9 of 10 pinned runs are positive;
  - these are the **pinned** series' numbers. The servers were pinned to one
    CPU, a harness choice this order did not name. The default series
    resolves the cost at 10,000 rows but not at 60,000, where its IQR crosses
    zero;
  - it is not linear in rows: 0.37 ns a row from 1K to 10K, and 1.5 ns from
    10K to 60K;
  - the walking `UPDATE`, which does the same walk, shows none (−3.7 µs);
  - **no line AP added runs on a rejected row**. `AcceptTupleAt`'s
    `fn_residual` test follows the residual's reject
    (`src/exec/step_vm.cpp:2280` at `c4b82e4`), and the column masks are
    compile-time. The cost is unattributed, and code layout is the reading
    left.
- **Using a function** (B only): `DATE(ts)` is about 18 ns per row dearer
  than the same `BETWEEN`, and never uses an index on `ts`.
- **Not measured:** `cores > 1`, `group` and `strict` durability, more than
  one session, and the parked-write path.

**What AP carries forward.**

- **The scan's per-row cost.** Recorded here, unattributed. No line AP
  added runs per rejected row, so `Step`'s larger size and code layout are
  the reading left.
- **B3**, a quiet wrong answer that predates AP:
  `docs/inflight/bugs/a-nested-uncorrelated-subquery-is-dropped.md`.
  Owner: none.
- **D1's fold, cover and monotone serving.** These are AQ's (AP-Q5,
  AR1 §9.2). AQ's order is unwritten.
- **The cabin optimizer's candidate pick.** It can now be a function's
  argument. This costs, and does not answer wrongly.
- **The `-j8` timeout** of `IdAllocationAcrossCores`, recorded in
  `known-gaps.md`, Testing.
- **Pre-existing, cited and not taken:** the `MutatePatternRow`
  version-filter cell, and `RecordTrail`'s unreachable guard.
- **Recorded departures and leftovers of the stages.**
  - **AP-S1's rejected review findings:** the uncalled
    `FingerprintAccumulator::Reset()`, and `FingerprintOf`'s double hash.
  - **AP-S3.** Rule 1's sentence goes beyond AP-R3's wording and is not
    ratified text. "No `src/` change" holds for code only; two comments
    changed.
  - **AP-S4.** The test seam shipped as `ScopedTestFunction`, where AP-R4
    named it `RegisterFunctionForTest`.
- **Process.**
  - `010b8e0` went to `main` at the operator's word before its review
    returned. The review's fixes followed in `c4b82e4`.
  - `0c3268f` was pushed with `--no-verify` at the operator's word, so the
    hook would not load the host during the measurement. Its engine is
    `c4b82e4`'s, whose suite passed.
  - The results file is named for `0c3268f`, the HEAD it was written at,
    as AZ-S7's was. It is committed at `795c9f0`, whose trailer names
    Sonnet 5.5.

**The review** (`critics-developer`, on `16ecf20a`, read-only) confirmed
every commit id, every suite count and the headline numbers, which it
re-derived from the archive's JSON. It also confirmed bench rules 1, 2, 3
and 5, and the interleaving. Applied:

- **The named cost candidate was wrong.** It cannot run on a rejected row.
  Both files now say so.
- **AZ-R5's four commits** in the range are disclosed.
- **Three qualifiers** for the overhead summary: pinned, non-linear, and
  the walking `UPDATE`.
- **The departures list above.**
- **Suite citations** now carry their commits and the two first-run
  timeouts.
- **In the results file:** the default series' 10K resolution, the
  exclusion rule, and the excluded runs' values.

Rejected:

- **Regenerating a per-cell load table from `host.txt`.** Rule 4's evidence
  is in the archive; the file now states the departure instead.
- **The results file's bloat cuts:** the percentile table and the repeated
  "what the run teaches". The file is `ck-tester`'s, and the cuts change no
  fact.

It cuts no tag.
