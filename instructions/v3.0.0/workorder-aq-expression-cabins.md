# Work order AQ — expression and predicate Cabins, `expr_id`, cover serving

Written 2026-10-06 on `worktree-aq-s0-order` from `740d954a`
(`v2.7.0-615-g740d954a`), on the operator's *"먼저 AQ에 대한 작업지시서를
작성해줘"*: AR1 §14's second order. AR1 is ratified
(`raft-marks-2026-09-29.md` §5), AP is closed
(`workorder-ap-function-catalog-fetch-id.md` §6, AP-S5), and AY-Q11 made AQ
its own letter.

**Opened as AQ on 2026-10-06, with every §4 item marked as proposed**
(`raft-marks-2026-10-06.md` §2). Every stage waits for its own word. It cuts no tag; the v3.0.0 tag waits on M4.

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

On *"CLA 제안대로 진행하고 main에 push해줘"*, every §4 item was marked as
proposed (`raft-marks-2026-10-06.md` §2), and the order landed on `main`.

- **Open, each on its own word:** AQ-S1 to AQ-S5.
- **Out of AQ:** monotone serving and D3's sortable directory (AQ-Q4),
  automatic creation (AQ-Q5) and D1's fold (AQ-Q6).
- **Given before the review.** The marks were given while AQ-S0's review
  was running. A finding that would change a marked ruling goes back to
  the operator.

No stage has started.
