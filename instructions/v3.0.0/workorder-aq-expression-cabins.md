# Work order AQ — expression and predicate Cabins, `expr_id`, cover serving

Written 2026-10-06 on `worktree-aq-s0-order` from `740d954a`
(`v2.7.0-615-g740d954a`), on the operator's *"first, write the work order
for AQ"*: AR1 §14's second order. AR1 is ratified
(`raft-marks-2026-09-29.md` §5), AP is closed
(`workorder-ap-function-catalog-fetch-id.md` §6, AP-S5), and AY-Q11 made AQ
its own letter.

**Opened as AQ on 2026-10-06, with every §4 item marked as proposed**
(`raft-marks-2026-10-06.md` §2). Every stage waits for its own word. It cuts no tag; the v3.0.0 tag waits on M4.

**Taken up on 2026-10-14 on the operator's standing go-ahead
(go-ahead-achieving-milestone)**, on `worktree-aq-expression-cabins` from
`7c234766` (`v3.2.0-44-g7c234766`), `instructions/v3.2.0/raft-marks-2026-10-14.md`
§14: that word is every stage's own. **AQ-R1's read-by-length mechanism is
superseded** by the operator's standing order of 2026-10-07 (no superblock
backward compatibility), which postdates AQ-Q1's mark: superblock 26 refuses
every older volume, and no 28-byte row is read. AQ-Q7..Q16 (§4) are CLA's
proposals adopted on the standing go-ahead, not the operator's marks.

## 0. What AQ is

AR1 §14 gives AQ this content and these dependencies:

> expression and predicate shapes, `expr_id`, `SysCabinRow` revision,
> cover-based serving, C3/C7/§12 prose — depends on AP; AN for any
> cross-core serve

Both dependencies are met:

- **AP** closed at AP-S5. It built the function catalog, purity, and the
  function conjunct (`include/kds/exec/functions.hpp`,
  `include/kds/exec/step_chain.hpp`'s `FunctionPredicate`).
- **AN** closed long ago, and the Cabin store has been the instance's
  since AT-S7.

The ratified items AQ builds against:

| AR1 item | what | class |
|---|---|---|
| AR1-4, §5 | a Cabin is a key function `F: tuple → K`; column, expression and predicate are shapes of `F` (chain is AR's) | design |
| AR1-5, §6 | identity `(rel_oid, expr_id)`; `expr_id` is the fingerprint of `F`'s span with literals as shape, columns as schema positions | design |
| AR1-6, §8 | C3 struck; C7 moves to the expression; `CREATE CABIN ON <rel> (<F>)` | design |
| AR1-7, §9 | §12's class table gains the source-of-completeness axis; the advisory class stays Waystone (D1) | spec |
| AR1-8, §10 | the witness: "an input column of `F` changed" | design |
| D3 | 16 B of key held by value in a **sortable** directory; wider keys lose range serving | constant |
| D4 | `CREATE CABIN ON <rel> (<F>)`; the column policy stays per column | spec |
| D7 | `SysCabinRow` revised, `column_no` dropped, old rows recognised on read **by a mechanism the work order supplies** (AR1-V4) | format |

**These are not AQ's:** the chain shape, the composition witness, D5 and
Waystone's chain use of `(fetch_id, arg_hash)` belong to AR. D2 and D8 were
built by AP.

**Carried in from AP** (its AP-S5 row):

- **D1's fold (AP-Q5).** §1.6 finds AQ gives it no consumer either.
- **Cover and monotone serving**, which AP declared no field for.
- **The cabin optimizer's candidate pick**, which can name a function's
  argument. AQ-R7 fixes it.

## 1. Survey at `740d954a`

Read, not run. Every citation is to `740d954a`.

### 1.1 Everything that names a Cabin is column-bound

- **The catalog row.** `SysCabinRow` (`include/kds/catalog/rows.hpp:754`)
  is exactly 28 B (`:796`). It is `cabin_id`, `rel_oid`, `observed_ct`,
  `column_no` (u16), `origin` and `status`. `CheckSize` refuses any other
  length as `Corruption` (`src/catalog/rows.cpp`). `status` has no spare
  bit (`rows.hpp:809-821`), which is AR1-V4.
- **The cache.** `TableAccess` maps a column to its Cabin: `cabin_ids[col]`
  and `cabin_mask` (`include/kds/catalog/schema.hpp:258-291`). These are
  filled from the row's `column_no` (`src/catalog/catalog.cpp:2322-2325`).
- **The write hook.** `NoteCabinWrite`
  (`src/server/command_dispatcher.cpp:6746`) walks `cabin_mask` per column,
  coerces the column's value and appends under `(cabin_id, value)`. It has
  two callers: INSERT (`:5119`) and UPDATE (`:7465`).
- **The probe pick.** `CabinProbeOf` (`src/exec/step_compiler.cpp:962`) and
  `CorrelatedCabinProbeOf` (`:1006`) read `Step::residual` for `col = value`
  on a cabined column. `CabinProbe` (`include/kds/exec/step_chain.hpp:198`)
  carries `cabin_id`, `col_pos` and a value.
- **The recording walk** compares the probed column's decoded value
  (`src/exec/step_vm.cpp`, the `recording_here` arm).
- **The cabin optimizer's candidate.** `CandidateRef` is
  `(rel_oid, col_pos, cabin_id)` (`include/kds/stats/optimizer_signals.hpp:76`).
- **The grammar.** `CREATE CABIN ON t(col)` names one column
  (`src/parser/parser.cpp:2194`).

### 1.2 What is already shape-agnostic

- **The store.** `CabinKey` holds the key **by value**: `cabin_id`, the
  value's type, `int_val` and `str_val` (`include/kds/stats/cabin_store.hpp:154-168`).
  An expression's output is an `AstValue` like a column's, so the store, its
  partitions, the announce/banking discipline (`cabin.md` §6/§6a) and the
  caps (§8) need no change to hold `E(F(t))`.
- **The function conjunct.** `DATE(ts) = '...'` is already
  `PredicateKind::kCompareFunction`. It lowers to `fn_residual`, and is
  evaluated on every row a step accepts (AP-S4). AP-R4 item 2 rules it
  "never ... a Cabin probe ... Serving a function predicate from a structure
  is AQ's cover". **AQ is the order that lifts that clause**, for one case
  only (AQ-R5).

### 1.3 `expr_id` is a hash, and a Cabin is authoritative

AR1 §6 matches a statement's sub-span to a Cabin when the two `expr_id`s are
equal. `expr_id` is a 64-bit FNV value, so two different `F`s can share one.
A Waystone pays for such a collision with a miss. A Cabin pays for it by
serving one expression's set for another's - fewer rows, no error.

`cabin.md` §3's by-value rule, and §12.3's "found by hash and then confirmed
against the stored group key", are the engine's answer to this already.
AQ-R2 applies the same answer. **`[quiet-wrong]`**.

### 1.4 The witness, generalised, has two write sites and one rule

`NoteCabinWrite`'s two callers hand it the whole new row (INSERT's `body`,
UPDATE's row) and, for UPDATE, the previous one. So `F` can be evaluated
where the column's value is read today.

AR1 §10's "an input column of `F` changed" is the existing per-column
unchanged-value test (`command_dispatcher.cpp:6790-6806`), applied to `F`'s
inputs. **A write path that skips the evaluation leaves a set missing a
row**, which is §1's one break. So the two call sites are the whole
surface, and each needs a cell. **`[quiet-wrong]`**.

### 1.5 Predicate implication must be exact

AR1 §9.2 serves a predicate Cabin `p` to a statement `P` "when `p` is a
conjunct of `P` - syntactically, never by theorem".

- **If the recognition errs toward a match**, a statement whose `P` does
  not imply `p` is served `E(true)`, the rows passing `p`. It loses every
  row passing `P` and failing `p`. That is a quiet wrong answer.
  **`[quiet-wrong]`**.
- **If it errs toward no match**, it costs a scan.

AQ-R6 states the comparison as one of canonical lowered conjuncts.

### 1.6 D1's fold still has no consumer

`F` must be `kImmutable` (AR1 §10: it is evaluated on the write path). So no
`NOW()` can be part of a Cabin's key, and a probe value must be a literal
(the `CabinProbe` contract). A statement `DATE(ts) = DATE(NOW())` therefore
probes nothing. The D1 fold AP deferred here (AP-Q5) has no consumer in AQ
either.

### 1.7 Cover needs a function field AP did not declare

AR1 §9.2's cover source is `x ∈ [a, b]` with `F = f(x)` and `f.cover`. For
`DATE`, that maps a `ts` range to the days it touches. `FunctionEntry`
(`include/kds/exec/functions.hpp`) has no `cover`, because AP had no reader
for one.

The monotone source needs D3's sortable directory, and the store's
directory is a hash map today (`cabin_store.hpp`, `Partition`).

## 2. Rulings — CLA's proposals, marked as proposed 2026-10-06

**AQ-R1 — the catalog: a v2 `SysCabinRow` recognised by its length, and
`F`'s text in a relation of its own** (D7, AQ-Q1).

- **The v2 row** is fixed-width: `cabin_id` u64, `rel_oid` u64,
  `observed_ct` u64, `expr_id` u64, `shape` u8 (column / expression /
  predicate), `origin` u8, `status` u8, a reserved byte, and `key_width`
  u16. The `static_assert`ed layout and length are the stage's.
- **`Decode` dispatches on length.** A 28 B row is a v1 row: `shape` is
  column, and `expr_id` is computed on read from `column_no`, as AR1 §6
  defines it for `F = t.x`. No rebuild scan, and no superblock move. That
  is the mechanism AR1-V4 asked the order to supply.
- **Writes.** v1 rows are never rewritten. A new Cabin of any shape is
  written v2.
- **`F`'s canonical text** lives in a new catalog relation,
  `sys.cabin_exprs (cabin_id, expr_text)`, in user tuple format, as
  `sys.assertions` stores its text. The text is the lowered form: the
  function's name, column **positions**, and coerced literals. A column
  Cabin has no row there.
- **At load,** the catalog cache recompiles `F` from the text, and the
  compiled form is what AQ-R2 confirms identity against.

**AQ-R2 — `expr_id` is a lookup key; identity is the compiled `F`**
(`[quiet-wrong]`, §1.3).

- `expr_id` is AR1 §6's fingerprint: `F`'s span, literals as shape,
  columns folded as schema **positions** (AQ-Q2).
- A probe or a witness finds candidate Cabins by `(rel_oid, expr_id)`.
  It then **confirms** that the Cabin's compiled `F` equals the statement's
  compiled `F`: function entry, argument positions, operator, and coerced
  literal. A mismatch is no Cabin.
- `CREATE CABIN` refuses a second Cabin whose compiled `F` equals an
  existing one's. Two Cabins with one `expr_id` and different `F`s are
  legal and kept apart.

**AQ-R3 — `TableAccess` keeps the column map and gains a list.** The column
shape keeps `cabin_ids`/`cabin_mask` byte for byte, so the v1 path, its
write hook and its probe are unchanged. Expression and predicate Cabins are
`TableAccess::expr_cabins`: each holds the `cabin_id`, the shape, the
compiled `F` and its input-column mask. A relation with none pays one
`empty()` test on the write path.

**AQ-R4 — the witness for `F`** (`[quiet-wrong]`, §1.4).

- **Admission.** `CREATE CABIN ON t (F)` admits only `kImmutable`
  functions. Any other purity is refused `InvalidArgument` at its byte.
- **The hook.** `NoteCabinWrite` walks `expr_cabins` after the column map,
  at both callers:
  - INSERT evaluates `F(new)`. If `F(new)` is observed, it appends.
  - UPDATE evaluates `F` only if some input column changed (the input
    mask against the previous row). Then, if `F(new)` is observed, it
    appends to `E(F(new))` and leaves `E(F(old))` alone (AR1 §10's table).
- **NULL.** A NULL input makes `F` NULL, and NULL observes nothing, as
  `MakeValueKey` already refuses it.
- **Failure.** An evaluation error un-observes the value it was writing
  for, as the coercion failure at `:6776-6788` does today.

**AQ-R5 — serving an expression Cabin: the probe is a hint on top of the
function conjunct, which stays.** This is the one amendment to AP-R4
item 2.

- **When a step is a Cabin probe.** A function conjunct `F(cols) = literal`,
  whose `F` confirms (AQ-R2) against an expression Cabin of that step's
  relation, may make the step a `kCabinProbe`.
- **The conjunct is not moved.** It stays in `fn_residual` and is evaluated
  on every served row. That keeps `cabin.md` §4's "re-check the key"
  structural, and it keeps a step downgraded to a scan returning the same
  rows.
- **Rank.** The probe takes the rank the column Cabin probe has in the kind
  ladder (`step_compiler.cpp:1887`).
- **Still never** a pk key, a bound, an index key, a join key or a
  `BuildKey`. The correlated form (§4a) is not extended to `F`.
- **The recording walk** compares `F(row) = v`, the same evaluation the
  conjunct makes, so a recorded set and a served one are one function.

**AQ-R6 — the predicate shape: `E(true)` for a conjunction, served on an
exact conjunct match** (`[quiet-wrong]`, §1.5; AQ-Q3).

- **What `p` may be:** `CREATE CABIN ON t (c1 AND c2 ...)`, a conjunction
  over the relation of
  - plain comparisons with a literal (`col op literal`, `IS [NOT] NULL`),
    and
  - `kImmutable` function comparisons with a literal.

  No `OR`, `NOT`, subquery or column-to-column comparison is admitted.
- **The key** is one value, `true`.
- **The witness** evaluates `p(new)` and appends to `E(true)` when it holds
  and `true` is observed. UPDATE does this only when an input column
  changed.
- **Serving.** A step's conjuncts (residual and `fn_residual`) are lowered
  and coerced. A predicate Cabin serves it when **every** conjunct of `p`
  equals one of the step's, by the canonical comparison of AQ-R2 - same
  column positions, same operator, same coerced literal. Nothing is
  normalised (no `<` against `>` swap, no `BETWEEN` against two bounds).
- **Re-checking.** Every conjunct of the statement, `p`'s included, stays
  where it is and re-checks each served row.

**AQ-R7 — the cabin optimizer's candidate pick reads the residual only.**
`RecordOptimizerSignals` takes the lowest bit of `filter_columns`, which
includes function arguments since AP-S4. It now takes the lowest
non-pk column of an equality in `Step::residual`, so a function's argument
is never priced as a column Cabin. Automatic creation of expression and
predicate Cabins is AQ-Q5's.

**AQ-R8 — cover serving for `DATE`, and nothing else** (AQ-Q4).

- **The field.** `FunctionEntry` gains `cover`. For `DATE` it maps a
  timestamp interval `[a, b]` to the day keys `floor(a) .. floor(b)`,
  inclusive. An open bound (`<`, `>`) keeps its day: the day is a superset
  key, and the residual excludes the boundary row.
- **When it serves.** A step whose residual bounds `ts` on both sides -
  `BETWEEN`, or `>=`/`>` with `<=`/`<` against literals - and whose
  relation carries a `DATE(ts)` expression Cabin may be served from
  `E(d)` for every day `d` in the cover.
- **All or nothing** (AR1 §9.2). One unobserved day sends the statement to
  the walk.
- **Re-checking.** The `ts` bounds stay in the residual and re-check every
  served row.
- **The cap.** A cover wider than `cabin_max_values` days is not served.
  The proposal reuses that existing setting, adding no new name.
- **Deferred:** monotone serving and D3's sortable directory (§1.7).

**AQ-R9 — the docs.** `cabin.md` is rewritten:

- C3 is struck, and §2 and §11 become AR1 §8's sentences.
- §3 and §10 state `F` and the v2 row.
- §8.1's C7 moves to the expression.
- §12.1 gains AR1 §9's source-of-completeness axis, with the advisory
  class placed in Waystone (D1).
- `alter.md` gets AR1-V3's sentence (AQ-Q2).
- `parser-v2.md` and the manual get `CREATE CABIN ON t (F)`.

## 3. Stages

Each stage waits for the operator's word.

| stage | what | exit | size |
|---|---|---|---|
| AQ-S0 | This order; the index row | the files at the commit | S |
| AQ-S1 | **The catalog and the grammar** (AQ-R1, R2, R3) | <ul><li>`CREATE CABIN ON t (DATE(ts))` creates a v2 row and a `sys.cabin_exprs` row.</li><li>`SHOW CABINS` shows the text.</li><li>A v1 28 B row, planted, loads as a column Cabin with its `expr_id` computed. Column Cabins are byte-identical.</li><li>Refusals at their byte: a non-`kImmutable` function, an `F` reading only the pk, a duplicate compiled `F`, and a `NO CABIN` column as an input.</li><li>A planted second Cabin with a colliding `expr_id` and a different `F` is not confused with the first.</li><li>Mount reload recompiles `F`.</li><li>The cabin contract suite is green unchanged.</li></ul> | M |
| AQ-S2 | **The expression Cabin's witness, serve and record** (AQ-R4, R5, R7) | <ul><li>Red first: `DATE(ts) = d` walks today with an expression Cabin declared.</li><li>Green: it is served (`cabin_hits=`), and its reply is byte-identical to the walk's.</li><li>**`[quiet-wrong]` witness cells:** INSERT of a row into an observed day, UPDATE moving `ts` into an observed day, UPDATE of a non-input column (no append), on one core and across two (the rig). Each is followed by a served read compared with the walk.</li><li>The cabin contract suite gains expression-Cabin configurations.</li><li>**Mutation:** one write site's evaluation removed, killed by its cell; identity matched on `expr_id` alone, killed by the collision cell; the conjunct moved out of `fn_residual` when served, killed by a planted surplus entry.</li><li>AQ-R7's candidate cell.</li></ul> | L |
| AQ-S3 | **The predicate shape** (AQ-R6) | <ul><li>`CREATE CABIN ON t (state = 'closed' AND amount > 1000)`.</li><li>Served when the statement's WHERE carries both conjuncts exactly. Not served with one missing, a different literal, `>=` for `>`, or the conjuncts as a `BETWEEN`.</li><li>Every served reply is byte-identical to the walk.</li><li>The witness cells as AQ-S2's.</li><li>**Mutation:** the match loosened to any one conjunct, killed.</li></ul> | M |
| AQ-S4 | **Cover serving for `DATE`** (AQ-R8) | <ul><li>`ts BETWEEN a AND b` and the two-bound form are served across one, two and many days when all are observed, and walk when one is not.</li><li>Open and closed bounds at a midnight boundary.</li><li>A pre-1970 day.</li><li>The `cabin_max_values` cap.</li><li>**Mutation:** the cover's last day dropped, killed by the boundary cell.</li></ul> | M |
| AQ-S5 | **AQ's close** | <ul><li>A row per stage; what AQ carries.</li><li>The overhead measured once over the whole change. The column-Cabin write path and point reads must show none. The expression write path's cost is stated as the feature's, B only.</li></ul> | S |

## 4. Items for the operator

| # | item | class | CLA proposal |
|---|---|---|---|
| AQ-Q0 | **The letter**: whether this order opens as AQ | scope | yes. AR1 §14's second order, its dependencies met |
| AQ-Q1 | **D7's mechanism, and where `F`'s text lives** (AR1-V4) | format | **AQ-R1**: the v2 row recognised by its length, so v1 rows read as column Cabins with no rebuild and no superblock move; the text in a new user-format `sys.cabin_exprs`. Alternatives: a superblock 19 → 20 that refuses older volumes (AY-Q6 (C)'s precedent), or a new status value |
| AQ-Q2 | **AR1-V3: `expr_id` over column positions**, against `alter.md` AL3's "patterns are allowed to die" on `RENAME` | spec | positions, as AR1 §6 says. A Cabin is a declared structure with a maintained witness, and losing one to a rename is a different cost from losing a trail. `alter.md` gains the sentence saying why the two answer differently |
| AQ-Q3 | **The predicate shape's admitted forms and match rule** | spec, **quiet-wrong** | AQ-R6: conjunctions of literal comparisons, plain or `kImmutable` function, matched conjunct for conjunct on the canonical lowered form with no normalisation. A wrong match is a lost row, so the rule errs toward the scan |
| AQ-Q4 | **Cover's scope** | scope | AQ-R8: `DATE` alone, both-sided ranges, all-or-nothing, capped by `cabin_max_values`. Monotone serving and D3's sortable directory go to a later letter |
| AQ-Q5 | **Automatic creation of expression and predicate Cabins** by the cabin optimizer (AR1 §8: "when a D0 pattern's predicate-position `expr_id` crosses the sighting threshold") | scope | not in AQ. Declared Cabins first; the controller's pricing of an expression is its own work. `CABIN AUTO` stays a column policy until then |
| AQ-Q6 | **D1's fold, carried from AP-Q5** | cost | stays deferred: AQ gives it no consumer (§1.6). It moves with whichever letter first observes a `kStable` value |

### Items surfaced in the stages - adopted on the operator's standing go-ahead of 2026-10-14 (go-ahead-achieving-milestone)

| # | item | class | CLA proposal, adopted |
|---|---|---|---|
| AQ-Q7 | **AQ-Q1's mechanism against the standing order of 2026-10-07** | format | the order governs: `sys.cabins` rows are 37 B (`expr_id`, `shape` added), superblock 25 -> 26, a version-25 volume refused with no migration. Not a re-opening of AQ-Q1: its text-in-a-relation half stands, its read-by-length half is the case the later order decides. Costs: every existing volume stops mounting |
| AQ-Q8 | **D7's "`column_no` dropped"** | format | **kept for the column shape**, 0 for the other two. AQ-R1's own v2 layout leaves a column Cabin no other carrier for its column (it has no `sys.cabin_exprs` row), and AQ-R3 keeps the column path byte for byte. `key_width` is **not** added: its reader is D3's sortable directory, which AQ-Q4 defers, and a field with no reader is a promise nothing checks |
| AQ-Q9 | **Where `sys.cabin_exprs` lives** | format | a third declaration relation (`exec/declaration_relation.hpp`'s shape: `aux` = `cabin_id`, `name` = `cabin_<id>`) on **fixed page 15**, the retired `sys.ranges` root nothing creates, so `kCatalogOverflowFirst` does not move; oid 153, column oids 154..159 |
| AQ-Q10 | **AQ-R2's "compiled `F` equals"**, made concrete | spec, **quiet-wrong** | the **canonical text** of the lowered `F` is the identity: column positions, the function's catalog name, the operator, and every field of the literal after coercion (a uint64's digits without leading zeros); a predicate's conjuncts sorted by their text and de-duplicated, so `a AND b` is `b AND a`. The stored text is that string, and a load recompiles and re-canonicalises it, refusing `Corruption` when the two differ or the `expr_id` does not match. `expr_id` is FNV-1a over the text with every literal folded to its kind (AR1 §6), so `state = 'open'` and `state = 'closed'` share one and are kept apart by text |
| AQ-Q11 | **A dropped relation's texts** | gap | removed after the drop commits in autocommit, as its Transitions are (CN-4 T7); inside an explicit transaction they stay - the drop can still roll back its Cabins' rows - and are inert once it commits, since nothing reads a text no `sys.cabins` row names. Stated in `known-gaps.md` |
| AQ-Q12 | **The tuple key as an input** | spec | admitted beside another column (`id > 5 AND state = 'open'`); refused when `F` reads it alone (the S1 exit's refusal), and the write hook reads it from the tuple key in hand at INSERT. A key reading no column at all is refused too |
| AQ-Q13 | **A lock for an expression Cabin's DDL** (AQ-S1's review, B2) | spec, **quiet-wrong** | the relation `X`, as `CREATE INDEX` takes it (AT-S5e), for the expression and predicate shapes only. Without it a create compiled on one core could land its text after another core's `DROP COLUMN` found no Cabin reading the column, leaving a text that names a dropped column and fails every fill of the relation; and a writer's `IX`, held from before it reads the relation's Cabins until it decides, keeps a Cabin from appearing under a writer that compiled without it - the witness's premise in AQ-S2. The column shape keeps its unlocked path (known-gaps.md) |

| AQ-Q14 | **What an expression probe reports to the access statistics** (AQ-S2's review, B3) | spec | **no column**: the statistics are keyed by column (`SHOW ACCESS`, the controller's candidate), `F` is not one, and filing `DATE(ts) = d` under `ts` would merge it with a column Cabin on `ts`. The probe's shape is still recorded, as a `kCabinProbe` with an empty column set |

| AQ-Q15 | **A statement's `BETWEEN` against AQ-R6's "no `BETWEEN` against two bounds"** (AQ-S3's review) | spec, **quiet-wrong** | the match is on **lowered** conjuncts (AQ-R6's own wording), and a statement's `BETWEEN a AND b` lowers exactly to `>= a` and `<= b` (`LowerComparison`), so a Cabin declared as those two bounds is served by it - sound, since the lowering is the statement's meaning. AQ-R6's sentence is read as: no normalisation beyond that lowering, and no `BETWEEN` in a declaration (refused at `CREATE`). The S3 exit's "not served ... as a `BETWEEN`" holds for a Cabin whose bounds a `BETWEEN` cannot spell (`amount > 1000`), which is the cell's case |

| AQ-Q16 | **The cover arm's rank in the ladder** (AQ-S4's review; AQ-R8 is silent) | cost | **after the correlated Cabin arm**, before the build annotation: on a join's inner step a Cabin keyed per outer row serves one set at a time, where a cover would re-serve and re-sort its whole union for every outer row. Every index arm and the literal Cabin arms stay ahead of it. Speed only; no answer depends on the rank |

Three surfaces in this order are `[quiet-wrong]`: AQ-R2's identity, AQ-R4's
witness and AQ-R6's match. AQ-R2 and AQ-R4 are rulings here, and their
cells are the stages' exits. AQ-R6's form is AQ-Q3's.

## 5. Sequencing

- **AQ-S1 before every other stage.** Every stage needs the row, the
  identity and the cache list.
- **AQ-S2 before AQ-S3 and AQ-S4.** The predicate shape reuses its witness
  and serve, and cover serves expression Cabins.
- **AQ-S3 and AQ-S4 are independent** of each other.
- **AQ-S5 is last.** The overhead is measured once over AQ's whole code
  change (`raft-marks-2026-09-30.md` §8), from `740d954a` to the closing
  commit.

## 6. Row status

### AQ-S0 — written 2026-10-06

On `worktree-aq-s0-order` from `740d954a`, as this file and its index row.
§1's survey was read against `740d954a`, not run. No code, spec or test is
changed. The letter itself is AQ-Q0's.

### AQ opened, and §4 marked - 2026-10-06

On *"proceed as CLA proposed and push to main"*, every §4 item was marked as
proposed (`raft-marks-2026-10-06.md` §2), and the order landed on `main`.

- **Open, each on its own word:** AQ-S1 to AQ-S5.
- **Out of AQ:** monotone serving and D3's sortable directory (AQ-Q4),
  automatic creation (AQ-Q5) and D1's fold (AQ-Q6).
- **Given before the review.** The marks were given while AQ-S0's review
  was running. A finding that would change a marked ruling goes back to
  the operator.

No stage has started.

### AQ-S1 - the catalog and the grammar, landed 2026-10-14

On `worktree-aq-expression-cabins` from `7c234766`, on the standing
go-ahead (`instructions/v3.2.0/raft-marks-2026-10-14.md` §14).

- **Built.** `SysCabinRow` is 37 B with `expr_id` and `shape`
  (superblock 26, AQ-Q7, AQ-Q8); `sys.cabin_exprs` is the third declaration
  relation, on page 15 (AQ-Q9); `CREATE CABIN ON t (F)` parses a column, an
  immutable call or a conjunction (`ParseCabin` reads ahead and rewinds the
  lexer); `exec/cabin_expr` compiles `F` to its canonical text - the
  identity (AQ-Q10) - and recompiles it at load; `TableAccess::expr_cabins`
  is filled by the catalog's declaration loader (renamed from the transition
  loader) and fails shut; `SHOW CABINS` gains `shape` and `key`; `DROP
  COLUMN` restricts a column any Cabin's key reads, of any status; `DROP
  TABLE` removes the texts in autocommit (AQ-Q11); the expression shapes'
  DDL takes the relation `X` (AQ-Q13). The golden WAL log is re-pinned
  (bootstrap's three new catalog rows).
- **The exit, cell by cell** (`tests/cabin_expr_test.cpp`): a `DATE(ts)`
  Cabin creates a v2 row and a text row and lists its key; a predicate is
  canonical and its commuted, repeated spelling is the same key; a column
  Cabin is unchanged - its reply, its row, its mask; **a row of the old
  width is refused** rather than read (AQ-Q7 replaces the planted-v1-row
  cell); refusals at their byte for a non-immutable function, a key reading
  only the tuple key or no column, a duplicate `F`, a `NO CABIN` input and
  every form AQ-R6 does not admit; **two keys sharing an `expr_id` are kept
  apart** (`state = 'open'` and `state = 'closed'`, the natural collision:
  literals fold out of the hash) and `DROP CABIN` takes the one it names; a
  stored text that is not canonical is `Corruption`; a reopened catalog
  recompiles every key, every literal kind round-trips its text, and a
  renamed input keeps its Cabin.
- **Review** (`critics-developer`): B1 - a negative integer literal's signed
  digit text was stored and refused by the reader, disabling the relation;
  fixed (the text is kept only for a wrapped uint64 literal) with the
  every-literal-kind cell. B2 - a create racing `DROP COLUMN` across cores;
  fixed by AQ-Q13. B3 - `DROP COLUMN` checked serving Cabins only; fixed
  (every status, from the texts). B4 - `DROP CABIN` failed after its row was
  gone; fixed. B5 - a zero-argument key wrote an unreadable text; refused.
  B6 - a `sys.tables` page nested under the `sys.cabins` hold; the text's id
  is issued before it and the order is stated. S2-S6 applied (one row
  insert for both doors, one text lookup, texts removed by relation, the
  shape constants in `well_known.hpp`, one label). **Rejected: S1** (delete
  the evaluator `CabinKeyOf` until it has a caller) - AQ-S2 is its caller
  and lands next with its cells; `result_type_val`, the duplicate it named,
  is deleted.
- **Suite**: 3890/3890 at the stage's commit. Overhead not measured;
  measured at the milestone's close.

### AQ-S2 - the expression Cabin's witness, serve and record, landed 2026-10-15

On `worktree-aq-expression-cabins` from `f3e6d1fd`.

- **Built.** `NoteCabinWrite` evaluates every expression Cabin's `F` on the
  new row at INSERT, and at UPDATE when an input moved, appending under
  `F(new)` (AQ-R4); a key it cannot compute discards the Cabin's sets. The
  UPDATE path decodes whole rows for such a relation, and the sorted heap
  fill excludes it. `ExprCabinProbeOf` turns `F(cols) = literal` in
  `fn_residual` into a `kCabinProbe` carrying `CabinProbe::f`, matched by
  canonical text at the column Cabin's rank; the conjunct stays and
  re-checks every served row (AQ-R5). The recording walk compares `F(row)`
  (`RecordsRow`). The optimizer's candidate is the lowest equality column
  below 64 of `access_columns` (AQ-R7); an expression probe names none, and
  reports no column to the statistics (AQ-Q14).
- **The exit, cell by cell.** Red first: before the declaration the conjunct
  compiles to no Cabin probe (`AnObservedDayIsServedAndAnswersAsTheWalkDoes`'s
  first assertion; the stage's own red run at `f3e6d1fd` was **not
  executed** - at that commit no arm could make the probe). Green: served
  (`cabin_hits=1`) and byte-identical to the walk. Witness cells: INSERT,
  multi-row and column-list INSERT, UPDATE into the day (and from NULL),
  UPDATE out of it, UPDATE of a non-input column (no append), one core and
  across two (`CabinServeAcrossCores.AnExpressionCabinsDayIsWitnessedByAPeersWrites`),
  each read served and compared with the walk. The contract suite gains
  `ExpressionCabinsChangeNoReplyServingOrWitnessing` (btree and heap,
  re-read after every write against no Cabin at all). AQ-R7's cell
  (`AFunctionsArgumentIsNeverTheCandidate`).
- **Mutations, each killed**: the write hook's expression arm removed (9
  failures, among them the rig cell and the contract cell); the probe
  matched by function alone (`AnotherColumnsCallIsNotThisCabinsKey`); the
  served row's `fn_residual` skipped (the planted-surplus cell); the
  recording compared on the column (`AnObservedDay...`). **Identity on
  `expr_id` alone** has no mutant here: the probe never reads `expr_id` - it
  compares the text - and an expression `F` carries no literal to fold, so
  two expression keys share an `expr_id` only by a real hash collision. The
  collision cell for a serve is AQ-S3's, where predicates differing in a
  literal share one by construction.
- **Review** (`critics-developer`): no write path skips the witness (multi-
  row, KWP, column-list, parked writes, PURGE, rollback traced). B1 - the
  failure fallback called `Forget`, which drops an in-flight recording's
  mark so its walk could bank a set missing the row; now `Discard`, in the
  expression arm and in the column arm, whose old fallback un-observed an
  uncoerced key nobody looks up. B2 - the candidate could name a column
  past 64; fixed with S1 (`access_columns`). B3 - ruled as AQ-Q14. S2 (the
  input memo), S3 (a bounds check in `RecordsRow`), S4 (the decode mask
  names every input) applied. The test gaps it named - multi-row and
  column-list INSERT - are cells now; **the heap hint-failure re-record with
  `F` has no cell**, since SUS-1 refuses a new heap and the contract
  suite's heap relation is served without a hint failure.
- **Suite**: 3901/3901 before the review's fixes; after them 3901/3902 -
  the one failure `IdAllocationAcrossCores.TwoCoresWritingOneRelationIssueOneSequence`,
  a cell touching no Cabin that memory records timing out under `-j8` load,
  which then passed 5/5 alone. Overhead not measured; measured at the
  milestone's close.

### AQ-S3 - the predicate shape, landed 2026-10-15

On `worktree-aq-expression-cabins` from `1099ad88`.

- **Built.** `PredicateCabinProbeOf` serves a step from a predicate Cabin
  when every conjunct of `p` equals, by canonical text, one of the step's
  own lowered conjuncts in `residual` (a comparison with a literal, `IS
  [NOT] NULL`) or `fn_residual` (an immutable call's), with no
  normalisation beyond the lowering (AQ-R6, AQ-Q15); the key is `true`;
  every conjunct stays and re-checks. The witness and the recording are
  S2's, which already evaluate `p` whole. The literal text writes a uint64's
  digits only where they are its value, so a statement's `1000` and a
  stored one are one text. The two expression arms share `OwnCallTerm`,
  `ProbeFor` and an early return when the relation has no Cabin of the
  arm's shape.
- **The exit, cell by cell** (`CabinPredicateTest`): served with every
  conjunct, in either order and beside others; not served with one missing,
  another literal, `>=` for `>`, an equivalent but unequal bound, a
  `BETWEEN` that cannot spell `>`, an `OR` group, or another state - each
  reply compared with the walk on a twin relation; every write witnessed
  (INSERT single, multi-row and column-listed, UPDATE into and out of `p`,
  DELETE), re-read after each; no append for a non-input column; **two
  keys sharing an `expr_id` serve their own sets** (the serve half of
  AQ-R2's collision); a `BETWEEN`'s bounds served (AQ-Q15); leading zeros
  and `IS [NOT] NULL` conjuncts; a function conjunct inside `p`; an
  enclosing row's conjunct is not the step's own.
- **Mutation**: the match loosened to any one conjunct - killed (15
  failures).
- **Review** (`critics-developer`): no correctness bug - the sources of
  `Step::residual` (WHERE, inner-join equalities, propagation; outer joins
  refused; `bool_residual` unread), canonical-text collisions and the
  witness traced. The `BETWEEN` conflict ruled as AQ-Q15. Applied: the
  shared helpers, `TakesLiteral` exported, the early return, stale comments,
  `OnTwin` asserting it rewrote, and the cells it named (multi-row and
  column-list witness, IS NULL, leading zeros, the enclosing row). Rejected:
  moving the statement-side conjunct-to-text conversion into
  `cabin_expr.cpp` - it reads the compiler's slot and `IsOwnColumn`, and the
  text grammar already has one home in `CabinConjunctText`. The predicate
  shape's two-core witness is not a cell of its own: the arm it runs
  through is S2's, whose rig cell covers it.
- **Found, not caused by AQ**: an inner build buckets a conjunct on the
  enclosing row by the first outer row alone `[quiet-wrong]`
  (`docs/inflight/bugs/an-inner-build-buckets-outer-only-conjuncts-for-the-first-outer-row.md`),
  reproduced with no Cabin; not fixed here (`join-inner-build.md`'s).
- **Suite**: 3911/3911. Overhead not measured; measured at the milestone's
  close.

### AQ-S4 - cover serving for `DATE`, landed 2026-10-15

On `worktree-aq-expression-cabins` from `fce79b85`.

- **Built.** `FunctionEntry::cover` (DATE: `[floor(low), floor(high)]` in
  days); `CoverCabinProbeOf` makes a step whose residual bounds an expression
  Cabin's single argument on both sides (`>=`/`>`, `<=`/`<`, an equality;
  the tightest of each) a `kCabinProbe` with `CabinProbe::cover`, ranked
  after the correlated Cabin arm (AQ-Q16); `RunCoverStep` serves the union of
  every covered day's set when every one is observed, and walks - counting a
  miss - when one is not, or without asking when the width passes
  `cabin_max_values` (AQ-R8; no new setting). `ServeFromCabin` takes several
  sets, deduped across them and sorted into the walk's order. A cover
  records nothing, and a heap hint failure under one un-observes the
  failing day and walks.
- **The exit, cell by cell** (`CabinCoverTest`, and the contract suite's
  heap cell): one, two and many days served; one unobserved day walks, then
  serves once observed, and a write after observation is in its day's set;
  open and closed bounds at midnight and the last microsecond of a day; a
  day before the epoch; the value cap; a one-sided range and an inverted one
  walk, an equality is a one-day cover, an index on the argument outranks
  the Cabin; a miss is counted.
- **Mutations, each killed**: the cover's last day dropped (2 failures);
  the heap fallback's single-set branch (the review's bug) put back (6
  failures in `AHeapCoversFailedHintBanksNothing`).
- **Review** (`critics-developer`): **one `[quiet-wrong]` bug, fixed** - a
  one-day cover on a heap whose hint failed took the single-set path,
  `FallBackAndReRecord`, and re-recorded its day against the cover's empty
  probe value: an empty set banked and served as authoritative. The
  fallback now branches on whether the step is a cover, with the heap cell
  above. Applied: `ServedSet` by pointer (no key copy per column-Cabin hit),
  a `first > last` guard, the key check written once, the self-comparing
  oracle and a redundant line in the cells, and the compile-side cells it
  named. The ladder rank ruled as AQ-Q16. Not applied: sharing a bounds
  helper with `TupleKeyRangeOf` (the two differ in sign handling and in
  which bounds count; the review called it optional). **No cell covers a
  cover on a join's inner step** - a rank question with no answer at stake.
- **Also**: the canonical text writes `!=` for an inequality, the spelling
  the grammar takes, so `SHOW CABINS` prints a key an operator can write.
- **Suite**: 3919/3919. Overhead not measured; measured at the milestone's
  close.

### AQ-S5 - the close, 2026-10-15

On `worktree-aq-expression-cabins`.

- **A row per stage**: §6 above - S1 `f3e6d1fd`, S2 `1099ad88`, S3
  `fce79b85`, S4 `a5e33998`; the prose `c30f03ff`; the merge of
  `origin/main` (`43cabf49`, the column-name case work) `fecfb88a`, whose one
  code conflict put AQ's two column resolvers on `Schema::FindColumn`; the
  regression's fix `e53514f2`.
- **The overhead, measured once over the whole change**
  (`bench/v3.2.0/results-aq-close-v3.2.0-48-ga5e33998.md`, A `7c234766`
  `v3.2.0-44-g7c234766`, where AQ opened - not the order's `740d954a`, which
  predates every milestone since and would charge them to AQ - against B
  `a5e33998` `v3.2.0-48-ga5e33998`, release, interleaved with an A'
  control). **The column-Cabin write path and its served read show none**
  (an INSERT into a relation with a column Cabin -0.46 / -0.26 us, a
  served `sym = 'x'` +0.26 / +0.35 us, inside the spread; the served read at
  10,000 rows, prepared, +1.5 us unresolved). **One regression found and
  fixed in the milestone**: the every-row `UPDATE` of a relation with **no**
  Cabin cost +29.5 / +23.0 us (+4.0 / +2.9 %, about 20-30 ns a row) - the
  per-row `NoteCabinWrite` call, whose body AQ grew; `e53514f2` asks once per
  statement and skips the call where no Cabin witnesses. Re-measured (§8 of
  the file, 12 pairs): the fixed build reads inside A's spread on both paths
  (-5.3 / -4.8 us), its p0 floor at A's where the unfixed one sat 14-16 us
  above; the fix's own gain over the unfixed build is not resolved against
  the noise, and the unfixed build reproduced about half its close-run
  delta. The point `SELECT` by tuple key reads inside. **The expression
  write path's cost, B only** (the feature's): a `DATE(ts)` Cabin adds +0.73
  / +0.92 us to an INSERT, about 0.5 us over a column Cabin's, a direction at
  the size of the noise floor; served `DATE(ts) = d` costs 39.9 us against
  115.9 us walked. Cut for the hour: no scan-row census, no predicate or
  cover shape timed, no profile, 3-12 pairs per cell.
- **What AQ carries out**: AQ-Q4's deferrals (monotone serving, D3's
  sortable directory), AQ-Q5 (automatic creation of expression and
  predicate Cabins), AQ-Q6 (D1's fold); `known-gaps.md`'s AQ section (texts
  left by a transactional `DROP TABLE`, the column shape's unlocked DDL);
  the heap hint-failure re-record under `F` has a cell only for a cover
  (`AHeapCoversFailedHintBanksNothing`); and the inner-build `[quiet-wrong]`
  AQ-S3 found, not AQ's
  (`docs/inflight/bugs/an-inner-build-buckets-outer-only-conjuncts-for-the-first-outer-row.md`).
- **Review of the close's code** (`critics-developer`, the merge's
  resolution and `e53514f2`): no bug; `witnessed` is exactly the per-row
  test it replaces. Applied: one `CabinsWitness` for the early return and
  the skip, the reply naming the column as declared, `pos` for the
  position, and a cell that a key's columns resolve ignoring case.
- **Suite**: 3936/3936 at the close. The first run hit a full disk
  (`DropTableReclaimCrashTest`, "No space left on device" in `/tmp`): the
  close measurement's server data files, 16 GB under
  `/home/cdkbs/bench-runs/aq-close/wd-*`, were removed and the suite re-run.
- **Not merged to `main`, not pushed, not tagged**: those wait for the
  operator's word.
