# Work order AP — `fetch_id`, rule 0′ and the function catalog

Written 2026-10-02 on `worktree-ap-s0-order` from `4012617`
(`v2.7.0-573-g4012617`). The operator's word was *"b"*, answering CLA's
three options with *"Write AP's work order (AP-S0) from AR1 §14"*. AR1 is
ratified (`raft-marks-2026-09-29.md` §5), and AZ-Q5 settled AP first
(`raft-marks-2026-09-30.md` §16). AR1's status line says that settlement
*"opens no letter"*; the word above is what opens this one.

**This order is written, not opened.** Its §4 items are unmarked, and every
stage waits for its own word. It cuts no tag; the v3.0.0 tag waits on AR0
§8's chain through M4 (`raft-marks-2026-09-26.md` §4).

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

Nothing here touches Cabin.

## 1. Survey at `4012617`

Read, not run. Every citation is to `4012617`.

### 1.1 The grammar has no scalar function and no expression

The manual states it as a choice. It says *"The grammar has no
expression tree at all — no arithmetic, no `OR`/`NOT` nesting, no function
calls outside the five aggregates. A smaller grammar every statement of
which executes predictably was chosen over a larger one"*
(`manual/sql/sql.md:907-911`). The AST agrees:

- A WHERE conjunct is a column against a literal, a column, a subquery or
  a `BETWEEN` pair (`include/kds/parser/ast.hpp:197-227`).
- A select item is a column or one of the five aggregates (`:520-540`,
  `AggFunc` at `:509`). AR1 §2 keeps the aggregates out of the catalog.

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

**Its one statement-path reader is the replay lookup**
(`src/server/command_dispatcher.cpp:6526`). `stmt_class` is read only for
display: `SHOW PATTERNS` at `:1611` and the `sys.patterns` view at
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

- The row is exactly 41 B (`rows.hpp:605`), and `CheckSize` refuses every
  other length as `Corruption` (`src/catalog/rows.cpp:10-15`).
- A 49 B row is unreadable by the running build, and a 41 B row is
  unreadable by the new one.
- AR1 supplied no mechanism for the change, which is the same gap AR1-V4
  found for D7.
- The precedent for such a change is the superblock moving
  (`include/kds/server/superblock.hpp:242`, now 19). AY-Q6 (C) moved it
  18 → 19 and refused every older volume.

AP-Q2 puts the two shapes to the operator.

### 1.3 The instance key reaches the waystone page

The trail's instance key is `{pattern_id, arg_hash}`
(`command_dispatcher.cpp:6509`). The page header stores it
(`include/kds/stats/waystone.hpp:71`), and the walk compares it
(`src/stats/waystone_page.cpp:132`). Re-keying the trail on `fetch_id`
therefore changes what that header word means: it would hold a `fetch_id`.
After the change, **every trail recorded before it misses**. Its header
holds a `pattern_id`, every trail is a `SELECT`'s (below), and a `SELECT`'s
`fetch_id` never equals its `pattern_id`, because its select list is never
empty. The old rows stay, unreachable, like rows of a retired fingerprint
version, and retention is unbuilt. The miss costs the descents the trail
would have saved and no result (invariant 8).

**Recording and replay are SELECT-only.**

- Replay is read in `HandleSelect` (`command_dispatcher.cpp:6494-6533`).
- `RecordExecution` (`:6707`), which calls `RecordTrail`, has three
  callers, all `SELECT` paths: `RunAggregated` (`:6168`), `RunAnalyze`
  (`:6265`) and `HandleSelect` (`:6697`).

So `fetch_id == pattern_id` for a non-`SELECT` (§1.4) preserves no row,
because no non-`SELECT` row exists.

### 1.4 `fetch_id`'s second argument stream has nothing to carry

AR1 §4 proposes two additions:

- a second argument stream that skips select-list literals;
- a BIND tag for a select-list `?`.

The select list holds neither (§1.1). So the argument half of the trail
key is `arg_hash` unchanged, and `fetch_id` needs one more FNV state in
`FingerprintAccumulator` (`include/kds/parser/fingerprint.hpp:273-307`).
It needs no second argument stream. The skip rule AR1 §4 states is
"between `SELECT` and the first `FROM` at parenthesis depth 0". A
subquery's select list therefore stays shape, as it must:
`IN (SELECT b FROM u)` and `IN (SELECT c FROM u)` fetch different
values. For every leading word but `SELECT`, nothing is skipped, so
`fetch_id == pattern_id`.

### 1.5 Sharing one trail across select lists is sound by construction

A compiled chain is `f(shape, catalog)` (`src/exec/step_compiler.cpp:256`,
`:576`). Two patterns of one fetch may compile different chains: an
aggregated select list adds a fold, and whether the step numbering moves
with it was not read. **That can cost a miss and cannot produce a row:**

- Replay looks an entry up by `(step_id, pk)`, with the key evaluated from
  the current statement's frame (`src/exec/step_vm.cpp:572`, `:601`;
  `Find` at `:634`).
- Rule 1 checks the entry's `rel_oid` against the query's step
  (`docs/spec/waystone-concpets.md` §2).

An entry recorded under the other chain's step numbering is therefore
either a valid location of that pk in that relation, or a miss. This is
AR1-V1's argument reaching one case further. AP-S2's cells must show it
rather than assume it.

### 1.6 Rule 0′ still holds in the code, and the spec still says less

AR1-V1's reading re-verifies at `4012617`:

- `IsTrailReplayable` admits `kLookup` and `kProbe` only
  (`include/kds/exec/step_chain.hpp:164`).
- Both replay call sites pass the key derived from the frame
  (`step_vm.cpp:572`, `:601`).

`waystone-concpets.md:39` still scopes rule 0 to *"any join replay"*. D8's
wording change is unbuilt.

## 2. Rulings — CLA's proposals, unmarked

**AP-R1 — `fetch_id` is one more FNV state in the accumulator.**

- **Rule.** The state folds every token `pattern_id` folds, except those
  after a leading `SELECT` and before the first `FROM` at parenthesis
  depth 0.
- **Rides with the parse.** It runs on the same `Feed`, so nothing lexes
  twice. `FingerprintOf` returns it too, so there is one set of rules
  (`fingerprint.hpp:246-258`).
- **`Fingerprint` gains `fetch_id`.** `pattern_id`, `arg_hash`,
  `literal_count` and `param_count` are unchanged. `kFingerprintVersion`
  stays at 1.
- **One version covers both hashes.** From this stage on, a change to
  `fetch_id`'s skip rule is a bump under the rule at `:144-171`, and that
  file's comment says so.
- **The golden corpus** pins `fetch_id` beside every `pattern_id` it
  already pins. Those `pattern_id` values must not move.

**AP-R2 — the trail keys on `{fetch_id, arg_hash}` and statistics keep
`pattern_id`** (shape per AP-Q2).

- **Moves to `fetch_id`:** the replay lookup (`command_dispatcher.cpp:6509-6526`),
  `RecordTrail`, the recorder's register/touch/claim calls, and the
  waystone page header's key word.
- **Keeps `pattern_id`:** the optimizer's `NoteExecution` (`:6864`) and
  `sys.access_stats`.
- **`ANALYZE` prints both.** Its `pattern_id=` (`:6289-6298`) exists so
  that a statement can be matched to the row `SHOW PATTERNS` lists. Once
  the row is keyed by `fetch_id`, that match needs `fetch_id=` beside it.
- **The row's `stmt_class`** is the class of whichever pattern first
  recorded the fetch. Nothing reads it except for display (§1.2), and the
  display says so.

**AP-R3 — rule 0′ as wording** (D8 as AR1-V1 amends it).

- **`waystone-concpets.md` rule 0.** "Mandatory before any join replay"
  becomes "mandatory before any replay, the driving step included". The
  mechanism is stated as the index key `(step_id, pk)`, not as a check.
- **`parser-v2.md` I17** gets the same scope sentence.
- **No engine change.** One cell pins the property at the driving step:
  the same instance key with a different derived driving key misses. If
  `waystone_contract_test.cpp` already holds such a cell, it is cited
  instead of added.

**AP-R4 — the function catalog, built with its first functions and not
before them** (AP-Q1). The catalog, purity and the determinism class land
in the same stage as the first entries that use them, in this order:

1. **The catalog.** Entries are compiled into the binary. Each declares
   its purity, and an entry that declares none is `kVolatileRow` (D2): the
   struct's default value, which a test pins.
2. **The predicate side.** The first expression the grammar admits is a
   function call on the column side of a WHERE conjunct, compared to a
   literal or `?`: `F(col, …) op value`. It lowers to a conjunct the step
   chain filters on.
   - It is never a step key, an index key or a Cabin probe. Serving a
     function predicate from a structure is AQ's cover.
   - Its NULL handling is `null.md` §4's tri-state, collapsed at the one
     site.
3. **The determinism class** is computed at parse, over predicate
   position only (AR1 §3):
   - **D1:** a `kStable` value is folded into the instance key at BIND, in
     argument order, as a `?` would be.
   - **D2:** the statement is neither registered nor recorded.
4. **Refusals.** A function in the select list, `ORDER BY` or `GROUP BY`
   is refused `NotImplemented` at its position, and so is a nested call.
   A name not in the catalog is refused `InvalidArgument` at its position,
   as an unknown column is.

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
| AP-S1 | **`fetch_id`** (AP-R1) | <ul><li>Unit cells: two select lists over one `FROM … WHERE` converge.</li><li>Two subquery select lists do not.</li><li>An aggregated and a plain select list converge.</li><li>Every non-`SELECT` leading word gives `fetch_id == pattern_id`, including BI5's multi-row `INSERT`.</li><li>The golden corpus with `fetch_id` added and every `pattern_id` unchanged.</li><li>**Mutation:** depth ignored in the skip, killed by the subquery cell.</li></ul> | S |
| AP-S2 | **The trail on `fetch_id`** (AP-R2, AP-Q2's shape) | <ul><li>Red first: `SELECT a …` records and `SELECT b …` with the same arguments replays nothing today.</li><li>Green: it replays, and its result is byte-identical with replay off (the waystone contract suite, extended).</li><li>An aggregated and a plain statement over one fetch, both orders, results identical to replay off (§1.5).</li><li>A pre-change trail misses and records again.</li><li>The optimizer's signals still split by `pattern_id`.</li><li>**Mutation:** the lookup back on `pattern_id`, killed by the red cell.</li><li>`waystone-concpets.md` §1/§3/§5, the `sys.patterns` view and `SHOW PATTERNS` per AP-Q2, and `manual/` restated.</li></ul> | M |
| AP-S3 | **Rule 0′** (AP-R3) | <ul><li>The two spec sentences.</li><li>The driving-step cell, or its citation.</li><li>No `src/` change.</li></ul> | S |
| AP-S4 | **The function catalog and the determinism class** (AP-R4) - **gated on AP-Q1 naming the first functions** | <ul><li>Per function: its evaluation, its NULL and type refusals, each with a position.</li><li>A D0 statement with an immutable function records and replays.</li><li>A D1 statement with a `kStable` function records under a per-execution instance and never replays another execution's trail.</li><li>A D2 statement (a test-only `kVolatileRow` entry, unless AP-Q1 ships one) is never registered or recorded.</li><li>The default-purity cell.</li><li>The refusals of AP-R4 item 4.</li><li>**Mutation:** the D2 gate removed, killed by the D2 cell; the default purity flipped to `kImmutable`, killed by the default-purity cell.</li><li>`parser-v2.md`, `fingerprint.hpp`'s comment on functions, `manual/sql/sql.md:907-911` rewritten, and `types.md` per function.</li></ul> | L |
| AP-S5 | **AP's close** | <ul><li>A row per stage.</li><li>What AP carries.</li><li>The overhead measured once over the whole change. A second FNV state runs on every token of every statement, so the cell is the parse-bound point `SELECT`.</li></ul> | S |

## 4. Items for the operator

| # | item | class | CLA proposal |
|---|---|---|---|
| AP-Q0 | **The letter**: whether this order opens as AP | scope | yes. It is AR1 §14's first order, and AZ-Q5 settled it first |
| AP-Q1 | **The first functions, and where they may appear**. The grammar has none, and the manual states that as a choice (§1.1). Without a function, AP-S4 builds a catalog nothing calls | scope, user-visible | AP-S1..S3 proceed without it, and AP-S4 waits for this mark. Two candidates, each an example AR1 itself gives: `DATE(timestamp) → date`, `kImmutable` with a cover (§9.2's example, and the first thing AQ's expression shape needs); and `NOW()`, `kStable` (§2's example, the D1 path). **No `kVolatileRow` function ships.** D2 is exercised by a test-only entry, since a row-random predicate has no OLTP use. Position: the column side of a WHERE conjunct only (AP-R4) |
| AP-Q2 | **D6's shape** (ratified as "`SysPatternRow` gains `fetch_id`") | format | **amend D6: key the existing row by `fetch_id`, with no layout change.** §1.2 shows the row exists only to hold a trail root. Keyed by `fetch_id`, one root per fetch holds by the key's uniqueness, the superblock stays at 19, and the change is invisible to the format. The `sys.patterns` view's and `SHOW PATTERNS`' `pattern_id` column becomes `fetch_id`, since that is what it then holds. The alternative as ratified: a 49 B row and a format mechanism AR1 did not supply, either the superblock 19 → 20 refusing every older volume (AY-Q6 (C)'s precedent) or a length-dispatching `Decode`. Plus a rule for which of several rows holds a fetch's root |
| AP-Q3 | **A function name not in the catalog**: `InvalidArgument` or `NotImplemented` (a wire surface, `protocol.md` §11) | user-visible | `InvalidArgument`, as an unknown column is. The parser cannot tell a function KDS lacks from a misspelling, and `NotImplemented` would claim the first |

AR1's ratified D2 and D8 are both `[quiet-wrong]`. AP builds them as
ratified (AP-R4, AP-R3) and re-decides neither.

## 5. Sequencing

- **AP-S1 before AP-S2.** S2 keys on the hash S1 produces.
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

The operator's word is recorded here rather than in a
`raft-marks-2026-10-02.md`. A file of that name exists untracked in the
worktree `az-q3-release-any-failed-check`, which another live session
holds. Whichever lands second carries this word as a section of it.
