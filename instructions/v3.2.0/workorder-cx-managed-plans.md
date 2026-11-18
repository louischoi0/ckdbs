# Work order CX - the plan as a managed object: implicit change an option, the first plan fixed by default, plans set by bindings, the engine an advisor

**Written 2026-11-18 and opened the same day on W14**, on `main` at `286889cc`
(`v3.4.12-12-g286889cc`), on the operator's words, recorded in English
(`raft-marks-2026-11-18.md` §1-§5):

- **W0**: *"Keep the 'the query is the plan' contract for now. But go in the
  direction of letting the user set and manage the plan directly according
  to bind variables. In any case, the database itself implicitly changing a
  query's plan is not allowed. Down to the low level, the plan becomes
  something a DBA or an AI agent manages, like the statistics model."*
- **W1**: *"Write the milestone."*
- **W2**: *"Put introduction priorities 1-6 into the milestone."* - the six
  priorities of CLA's optimizer proposal of the same morning, before W0:
  (1) rule-based rewrites, (2) access path selection, (3) data distribution
  statistics, (4) join method selection, (5) the plan cache, (6) the join
  order search. W0 turned (6) into an advisor; since W3 it is also
  `planned`'s plan source and a `fixed` first compile's. §3 maps each one
  to a stage.
- **W3** (2026-11-18, `raft-marks-2026-11-18.md` §3): *"For now, whether to
  allow implicit plan regressions or changes - plan changes made at the
  database level, driven by statistics according to the cost model and its
  safeguards - must be a database option the user chooses."*
- **W4** (the same turn): *"And if this flag is off, the first plan is
  fixed, and after that a DBA or an agent must be able to choose the plan
  per query. An administrator's plan choice is always possible, whatever
  this option is."*
- **W5** (the operator's answers to CLA's three questions, each CLA's
  recommendation): the first plan is the plan the cost model chose at the
  shape's first compile; the option is a boot key and an instance-wide
  administrator statement at run time, re-scoping `join_order`; the default
  is off - the first plan fixed.
- **W6** (2026-11-18, `raft-marks-2026-11-18.md` §4): *"CX-Q12..Q18 $!a
  push no verify"* - CX-Q12..Q18 marked (a).
- **W7** (2026-11-18, `raft-marks-2026-11-18.md` §5): the statistics
  features taken into an order of their own,
  `workorder-cy-managed-statistics.md`; S4 points there.
- **W10** (2026-11-18, `raft-marks-2026-11-18.md` §8): *"And when no
  capture was made - the capture deferred for lack of statistics, the
  plan made again at each execution of the SQL - the user must still
  always be able to create a plan and fix it."* - CX-R11.
- **W12** (2026-11-18, `raft-marks-2026-11-18.md` §10): CX-Q28 marked
  (a).
- **W14, W15** (2026-11-18, `raft-marks-2026-11-18.md` §16): *"go on
  with ... the capture, the features around `CREATE PLAN`, the
  interface, and CX-S2 onward"*. **CX opened.** Plans, captured and
  administrator's, are logged (the operator's answer). CX-Q2..Q11 and
  CX-Q19..Q27 are adopted as CLA proposed on the operator's standing
  go-ahead, not marked item by item; the stages run one after another,
  each with its review and the full suite, and stop before any push,
  tag or version.

**Amended 2026-11-18 on W3-W5**, still not opened. W3 turns W0's third
clause from an absolute rule into the option `join_order`, and W4-W5 give
its off value a meaning: the first plan is captured and fixed. Where W0 and
W3 disagree, W3 is the later word and stands.

**Claim tags**: `[source-read]` with `path:line` at `286889cc`; `[design]`
for reasoning about code not yet written; `[measured]` with its commit;
`[quiet-wrong]` where a wrong choice turns a correct answer into a wrong
one with no error; `[operator]` for W0-W7.

**The letter.** `[source-read]` CW is the last work order under
`instructions/v3.2.0/` at `286889cc`, and no file, branch or commit subject
fetched names a `CX-` token or a `workorder-cx` file. CLA's choice.

**CX against other orders and specs.** Since W3, CX **keeps CB-S2's and
CC-S3/S4's statistics-priced order as the `planned` value of `join_order`
and takes it off the default**: the default, `fixed`, runs the same search
once per shape and keeps its answer (`workorder-cb-join-planner.md`,
`workorder-cc-plan-statistics.md`, both CLOSED). Their estimator stays,
serves `planned` and the first compile under `fixed`, and is also the
advisor in CX-S7. CX amends `docs/spec/parser-v2.md` I12 around three plan
sources in order - an administrator's plan, a captured plan, the search or
the written order - per CX-R1. It leaves standing: equality propagation and CB-S1's range bounds
(§3, CX-S2), the decorrelation prohibition as an *engine* rule, the
statement-local inner build's mechanics, invariants 8 and 9, and every
trust-class rule. It is not part of AR0 §8's chain.

**A correction CX owes the conversation that produced it.** Before W0, CLA
told the operator that the contract held today and that no cost-based
optimizer existed. That was false. It was said without reading the tree,
and §1.1 is what the tree says. W0 was given as a decision to *keep* the
contract. On the tree, the decision is to *restore* it. CX-Q0 put that
difference to the operator. W3-W5 settled it in a third way: neither restore
nor keep the default, but an option whose default fixes the first plan.

---

## 0. What CX is

W0 and W3-W5 together have three parts, and CX applies all three to every
plan decision the engine makes:

1. **The option** `[operator]` (W3, W5). `join_order` says whether the
   engine may change a plan on its own, at boot and at run time:
   - `planned`: statistics-driven change admitted, CB/CC as built - the
     search runs at every compile under its margins, and a plan moves when
     the statistics generation moves;
   - `fixed` (the default): a shape's first compile runs the same search
     and **captures** its answer; every later compile of the shape runs the
     captured plan, whatever the statistics do (W4);
   - `written`: the written order, `f(text, catalog DDL state)`, as I12
     read before CB.
2. **The managed plan** `[operator]` (W0, W4). A DBA or an agent may attach
   a plan to a statement shape, **under every value of `join_order`**, and
   it outranks the option. The plan may have variants chosen by the
   statement's bound values, under a rule the plan itself states. The
   plan is a catalog object, like an index or an assertion: created,
   altered, disabled, dropped, exported and restored by explicit acts.
3. **No implicit change unless the option admits it** `[operator]`. Under
   `fixed` and `written` the engine never moves a statement from one plan
   to another on its own. What it may do there is **advise**: it computes a
   better plan and shows it, and a person or an agent decides whether to
   adopt it (CX-S7). Under `planned` the operator has chosen implicit
   change, and the margins of CC (`plan-stats.md`) are its safeguards.

**What "plan" means here** `[design]`, so the rule has edges:

- the order of a block's steps;
- each step's access arm - the kind and the structure that serves it
  (tuple key descent or key set, tuple key range, a named secondary index,
  a column Cabin, a walk);
- whether a step's statement-local inner build is armed;
- which sanctioned rewrites apply.

Which rows come back is never part of a plan. Every arm in the list
locates rows under invariants 8 and 9. A plan chooses where to look and
never what is visible.

### 0.1 Not in CX

- A hoisted hash join or a decorrelating rewrite chosen by the engine.
  I12 forbids it under every value of `join_order`: W3's option admits a
  statistics-driven order, never a rewrite. As a managed-plan option it is
  CX-Q9, gated on a consumer.
- Adaptive execution: switching plans mid-statement on observed rows.
- Any change to what an advisory structure (Waystone, a hint) may do.
  Those structures replace lookups, never searches, and choose no plan
  (`workorder-ca-background-advisory-optimizer.md` §0).

## 1. The tree as read at `286889cc`

### 1.1 The engine reorders joins today, by default

- `[source-read]` The setting `join_order` defaults to `planned`
  (`include/kds/server/expeditor.hpp:442-444`, `plan_join_order = true`).
  `written` is the only way to keep a statement's order
  (`src/server/expeditor.cpp:438-446`).
- `[source-read]` The dispatcher passes the plan options to `Compile` only
  when `planned` is set (`src/server/command_dispatcher.cpp:8905-8912`).
  The options carry the fetch id, the literals' hash and whether every
  value is known.
- `[source-read]` `CompileBlock` prices the written order and every
  candidate order with `EstimateChain`. It takes the cheapest candidate
  that beats the written order by a margin: `kStatsSwitchMargin` once a
  statistic is read, `kCostTieMargin` before that
  (`src/exec/step_compiler.cpp:2443-2492`). It records `reordered`
  (`:2531-2532`) and searches every order up to `kMaxPlannedRelations`
  relations, greedily above that (`:2540`).
- `[source-read]` `parser-v2.md` I12, as amended by CB and CC
  (`docs/spec/parser-v2.md:139-141`), states the plan as
  `f(shape, catalog, statistics generation, span bands, statement limits)`.
  The inner build's side has been the planner's choice since CB-S2.

**So a statement's plan today moves when the statistics move.** The
published snapshot changes generation when a band moves
(`docs/spec/plan-stats.md` PS5). That is the implicit change W0 forbade
and W3 makes the operator's choice (`planned`), off by default (W5).

### 1.2 The statistics are the planner's alone and nobody manages them

- `[source-read]` `stats::PlanStats` holds relation priors, join-subset
  posteriors `card(T)` and decline facts. It lives in memory, is emptied
  by a restart, is never logged and is never a catalog relation
  (`plan-stats.md` §1-§2). Only `EstimateChain` reads it for a plan (PS9).
  No command collects, locks, exports or imports it.
- `[source-read]` The engine keeps no per-column NDV, MCV or histogram
  (`workorder-cc-plan-statistics.md` §1, "The engine keeps no row count,
  NDV or histogram today").

### 1.3 The memo exists only to serve the search

- `[source-read]` `exec::PlanMemo` caches a searched block's order, keyed
  by the block, the literals' hash, the statistics generation, the catalog
  stamps and the two limits (`plan-stats.md` PS6). A statement with a `?`
  bound later is not memoized.

### 1.4 What is already a pure function of the text

- `[source-read]` The access ladder is `f(shape, catalog)` given each
  step's predecessors (`parser-v2.md` I12; `docs/spec/index.md` §9).
- `[source-read]` Equality propagation and CB-S1's range bounds add
  conjuncts and never remove or reorder one. Both are computed from the
  text alone (`parser-v2.md` I12, "Equality propagation is the one
  sanctioned predicate rewrite"; "Range bounds cross the same classes
  since CB-S1").
- `[source-read]` A literal's value does reach the default plan, but only
  as text. A negative or NULL tuple key literal names no key (I10), and the
  range span band enters the cost only on the planned path. So under
  `written`, the plan is `f(text, catalog)` with the literals inside "text".

### 1.5 Inputs to a plan that are neither text nor DDL

These are the cases §0's third part reaches besides §1.1. Each is an
item in §4.

- `[source-read]` **Controller-created Cabins.** The cabin optimizer
  creates and drops Observational Cabins in the background
  (`docs/spec/cabin-controller.md` §II.3-§II.5). The ladder takes a Cabin
  arm when a Cabin exists (`parser-v2.md` I10, `docs/spec/cabin.md` §4e).
  The controller is experimental and **off by default**
  (`cabin-controller.md:6`).
- `[source-read]` **The build decline.** An armed inner build that would
  bucket more than `join_build_max_rows` declines at run time and the step
  walks per outer row (`src/exec/step_vm.cpp:1193-1207`). The rows are
  the same, and the locate mechanism changes with the data.

### 1.6 The binding surface exists

- `[source-read]` The wire protocol has `C_PARSE` and `C_BIND`
  (`docs/spec/protocol.md:77-78`). **A bind is a text substitution**:
  `OnBind` decodes the values and substitutes them into the statement's
  SQL (`src/server/kwp_session.cpp:580-585`), so the compile sees
  literals, whether the client sent literals or `?` markers.
- `[source-read]` The fingerprint keeps the two forms on one `fetch_id`
  and separates them only in `arg_hash`. A `?` adds nothing to the
  argument stream (`include/kds/parser/fingerprint.hpp:32-34`). **So "the
  bound values" in W0 are the statement's argument values**, the literals
  in value-position order, however they arrived.
- `[source-read]` `$name` is refused by the parser everywhere
  (`include/kds/parser/ast.hpp:53-56`). `parser-v2.md:153`'s "`$param`
  propagates" is stale prose and is corrected with S0's I12 rewrite.
- `[source-read]` The one user-declared, statement-keyed object the
  engine ever had, `CREATE PATTERN` with typed `$` parameters, was
  withdrawn on 2026-08-31 (`src/parser/parser.cpp:1853`). Its grammar is
  the precedent CX-S1 reads, not code it revives.

## 2. Rules

- **CX-R1 - the plan's sources, in order** `[operator]` (W0, W3, W4). A
  statement's plan is, first match wins:
  1. the chosen variant of an enabled **administrator's plan** that
     matches it - under every value of `join_order` (W4);
  2. under `fixed`, its shape's **captured plan** (CX-R7);
  3. under `planned`, the search's answer at this compile (CB/CC as
     built); under `fixed` with nothing captured, the search's answer,
     which is then captured; under `written`, the written order.

  Under `fixed` and `written` no other input reaches a plan once it is
  chosen: statistics, the memo, advisory structures, load, time and core
  placement are excluded. Under `planned` the statistics are an input by
  the operator's choice (W3).
- **CX-R1a - "bound values" are argument values** `[source-read]`. W0's
  bind variables are the statement's literals in value-position order,
  whether sent as text or substituted at `C_BIND` (§1.6). A variant
  chosen by them is the plan's own rule, not the engine's choice, so it
  sits inside CX-R1.
- **CX-R2 - a plan never changes a result** `[quiet-wrong]`. A managed
  plan selects only among arms the ladder could legally take for that
  step. Legality is checked at every compile, not only at `CREATE PLAN`. A
  plan that names an arm the statement cannot use is a refusal or a
  fallback (CX-Q4), never a forced arm.
- **CX-R3 - exact matching** `[quiet-wrong]`. A plan matches a statement
  only on its fetch id, its relation count and every bound relation oid,
  compared exactly. This is CC's key discipline
  (`workorder-cc-plan-statistics.md` §2). A hash never matches alone. A
  collision must miss; it must never apply one statement's order to
  another's relations.
- **CX-R4 - the variant rule is total and pure** `[quiet-wrong]`. It is
  evaluated only over the statement's argument values (`ARG(k)`, the k-th
  value position, §1.6), in written order. The first match wins, and an
  `ELSE` is mandatory. A NULL argument matches no comparison and reaches
  `ELSE`. The rule reads no data page and no statistic.
- **CX-R5 - advice is never applied by the engine** `[operator]`.
  Adoption is a `CREATE PLAN` or an `ALTER PLAN`, issued by a person or by
  an agent's session. A capture (CX-R7) is not adoption of advice: it is
  the `fixed` value's one plan decision per shape, made at its first
  compile and never revisited by the engine.
- **CX-R6 - zero cost for the unmanaged point path** `[design]`. A point
  read or a tuple key chain leaves the compile before any plan lookup, as
  it leaves before the statistics today (`plan-stats.md` PS9). The lookup
  is one probe of the plan map (its protocol is CX-Q22), and only for a
  statement whose fetch id has a plan. The same holds for the captured map (CX-R7): a
  tuple key chain skips the search today and is never captured.
- **CX-R7 - a capture is once per shape, instance-wide** `[design]` (W4,
  W5). Under `fixed`, a planned block's first compile with no
  administrator's plan and no captured plan runs the search and installs
  its answer in the instance's captured map under CX-R3's exact key
  (`fetch_id`, relation count, written relation oids). Two cores that
  compile the shape at once race to install; the first install wins and
  the other core runs the winner's plan, so no shape runs two captured
  plans. The captured plan is the order and the build side - what the
  search decides (CX-Q12). It serves every binding of the shape (CX-Q13),
  a statement with a `?` bound later included - the reverse of the memo,
  which does not remember one (`plan-stats.md` PS6), because the key holds
  no literal. **Under a minted view (a DDL in flight in the transaction) no
  capture is made and none is read**; the search runs uncaptured, as the
  memo is not read there (PS6): an order priced against a catalog state
  that may roll back is not fixed.
- **CX-R8 - a capture writes nothing on the statement** `[design]`. A
  `SELECT` writes no log and no catalog page (CF's rule; a `READ ONLY`
  transaction writes no record, `txn.md` §1). The capture is in memory at
  the compile; a `system` task persists it as a logged `sys.plans` row with
  `origin = captured`, carrying whether its capture was informed (CX-Q25). A
  restart before that task ran loses the capture, and the shape is captured
  again at its next compile (CX-Q14).
- **CX-R9 - an administrator's plan outranks every value** `[operator]`
  (W4). `CREATE PLAN`, `ALTER PLAN` and `DROP PLAN` are admitted whatever
  `join_order` is, and a matching enabled plan is source 1 of CX-R1 under
  `planned`, `fixed` and `written`. A DBA or an agent overrides a captured
  plan with `CREATE PLAN` for the shape, the captured row kept beneath it
  (CX-S1), or discards the captured row with `DROP PLAN`, after which the
  next compile under `fixed` captures again - an explicit act, so inside
  CX-R1.
- **CX-R10 - the option is one switch for the instance** `[operator]` (W5).
  `join_order` is read at boot and changed at run time by an admin-classed
  `SET JOIN_ORDER planned | fixed | written`, which flips the one
  instance-wide value every core reads at its task boundary, as `SET
  CABIN_OPTIMIZER` flips the optimizer surface (AT-S8). No second setting
  names the same thing (CLAUDE.md, Working Rules).
- **CX-R11 - a shape that is not captured can always be fixed by hand**
  `[operator]` (W10). Whatever keeps a shape uncaptured - its capture
  deferred for lack of statistics (CX-Q19), no key that fits it
  (CX-Q20), the map's ceiling (CX-Q21), a minted view (CX-R7), or
  `join_order` being `planned` or `written` - a DBA or an agent can
  give it a plan and that plan stays: by writing one (`CREATE PLAN ...
  VARIANT ...`), or by fixing the order the search gives at that moment
  (`CREATE PLAN <name> FOR <statement> AS SEARCHED`, CX-Q28 (a)). Either is
  an administrator's plan, source 1 of CX-R1, so it needs no statistic
  to exist and waits for none. A shape running uncaptured is visible
  with its reason (`sys.statement_stats`' `plan_source`: `searched`,
  `deferred`, `unkeyed`, `over_ceiling`), so the one who would fix it
  can find it.

## 3. Stages

S0 and S1 are the foundation W0 and W4 need. S2-S7 are W2's priorities 1-6, in
that order. A stage opens on the operator's word, and each one closes on a
measurement where §6 names one.

### CX-S0 - the option and the capture (prerequisite)

- `join_order` is re-scoped to `planned | fixed | written` (CX-R10),
  default **`fixed`** (W5). `planned` is today's behaviour, unchanged;
  `written` is today's `written`. A boot value other than the three is
  refused naming the key, as today (`src/server/expeditor.cpp:438-446`).
- `SET JOIN_ORDER <value>`, admin-classed, flips the instance's one value
  (CX-R10). It is memory-resident: a restart reads the boot key again
  (CX-Q15). `sys.meta` shows the value in force.
- **The captured map** (CX-R7): instance-wide, keyed by CX-R3's exact key,
  first install wins. Under `fixed` a planned block's compile probes it
  after the tuple key chain's skip and before the search; a hit compiles
  the written order and then the captured one through the full ladder,
  which is what a `PlanMemo` hit does today (`plan-stats.md` PS6), so
  every refusal stays the written order's. A captured order that no longer
  compiles is never forced: the compile runs the written order and counts
  it (CX-Q16). A relation's oid being gone is not this case: CX-R3's key
  misses it (CX-Q24).
- Until S1 the map is memory only, so a restart re-captures every shape
  (CX-R8). S0's report states this as a fact of the stage.
- I12 is rewritten around CX-R1's three sources. CB's and CC's amendments
  stay as the description of `planned` and of a `fixed` first compile.
  `plan-stats.md` PS9 gains: "under `fixed`, a fact is read by a shape's
  first compile and by the advisor, never by a later compile of the same
  shape".
- The search, `EstimateChain` and `PlanStats` keep a plan consumer at every
  value but `written`: `planned`'s every compile and `fixed`'s first. The
  steward keeps running (CX-Q2 (a), now forced by W5).
- `ANALYZE` prints `order=captured` for a captured plan, with whether its
  capture read a published fact (`informed=0|1`). The generation number is
  an in-memory counter a restart resets (`plan-stats.md` PS5), so it is not
  persisted as a comparable value (CX-Q25).
- **Expected cost** `[design]`: under `fixed` a shape runs the plan
  `planned` would have chosen at its first compile. **That compile is
  data-free until the steward's first publish** (`plan-stats.md` §1: a
  restart or a restore empties the store, and a publish comes at most once
  a second), so every shape first compiled before it is fixed on the
  data-free model's order, and warm-up does not repair a capture. Until S1
  persists captures, that repeats at every restart. What decides how many
  shapes this reaches is CX-Q19; what removes its cause is statistics that
  survive a restart (`workorder-cy-managed-statistics.md`, CY-S1). What
  `fixed` gives up beyond that is every later improvement the statistics
  would have made, and what it buys is that no plan regresses on its own.
  §6 measures both.

### CX-S1 - the plan as a catalog object, with bind-value variants (foundation)

- **`sys.plans`** `[design]`: one row per plan, stored like
  `sys.assertions` (logged, `source_text` form). It holds the name, the
  statement text with `?` in its value positions, the fetch id, the relation oids in
  written order, the variant list, the state (`ENABLED`, `DISABLED`,
  `INVALID`), its creator, its **origin** (`admin` or `captured`), whether
  a captured row's compile was informed (CX-Q25), and a reference list (indexes
  and Cabins it names). One shape holds at most one row of each origin; an
  `admin` row outranks a `captured` one (CX-R9).
- **Captured rows persisted** (CX-R8): a `system` task drains the captured
  map into `captured` rows, logged, off the statement. A captured row is
  named by the engine (`captured_<fetch_id>`), has one variant (`ELSE`) and
  is read at mount into the instance's captured map. `DROP PLAN` on it
  re-arms the capture; `CREATE PLAN` for its shape adds the `admin` row
  beside it, so a `DROP PLAN` of the admin row falls back to the captured
  one, never to a fresh search (CX-Q17).
- **Grammar** `[design]`, the precedent being the withdrawn
  `CREATE PATTERN` (§1.6):

  ```
  CREATE PLAN p_loans_by_user FOR
    SELECT ... FROM users u JOIN loans l ON l.user_id = u.id
    WHERE u.id BETWEEN ? AND ?
  VARIANT narrow WHEN ARG(2) - ARG(1) <= 64
    ORDER (u, l) ACCESS u TUPLE_KEY, l INDEX loans_user_idx
  VARIANT wide ELSE
    ORDER (l, u) ACCESS l SCAN, u TUPLE_KEY BUILD u OFF;
  ALTER PLAN p ENABLE | DISABLE;  ALTER PLAN p REPLACE VARIANT ...;
  DROP PLAN p;  SHOW PLAN p;  -- SHOW as sugar over SELECT, BO-R3
  ```

  Whether `WHEN` admits arithmetic over arguments or only
  `ARG(k) op constant` is CX-Q5.
- **Validation at `CREATE`**: each variant compiles under the ladder's
  legality rules with representative bindings (CX-R2). A variant that
  cannot compile refuses the `CREATE`.
- **Dependencies**: `DROP INDEX` or a Cabin drop that would leave an
  enabled plan's arm unservable is CX-Q4.
- **Literal statements**: a bind arrives as literals anyway (§1.6), so one
  plan serves the shape whether it arrives through `C_BIND` or as text.
  The variant is chosen from the literals at the compile. No protocol
  change is needed.
- **`ANALYZE`** prints `plan=<name>/<variant>`, `plan=captured`, or the
  reason there is neither (CX-R11): `plan=searched`, `deferred`,
  `unkeyed`, `over_ceiling`, `written`.
  `sys.statement_stats` gains `plan_name` and `variant`.
- **Agents**: `sys.plans` is an ordinary table (BO-R3), so an agent reads
  it, the advice (S7) and ASH (CJ) through SQL, and acts through the same
  DDL a DBA uses. Nothing agent-specific is added.

### CX-S2 - priority 1: rewrite rules, split by whether they are text-pure

- **Inventory** `[design]`: every rewrite the compile performs, classed
  (a) pure in the text and append-only, or (b) changing read scheduling.
  At `286889cc`, equality propagation, CB-S1's range bounds and the false
  uncorrelated `EXISTS` short-circuit are class (a) (§1.4).
- **Class (a) stays automatic**: it is computed from the text alone under
  every value of `join_order`, so it moves no plan on its own (CX-R1).
- **Class (b) is never automatic**. Subquery unnesting and semi-join
  conversion may exist only as a managed-plan option (`REWRITE UNNEST
  (subquery k)`), and only once a scenario statement is named as its
  consumer (CX-Q9).
- Predicate pushdown into the decode is OPT-001's mechanics, not a plan
  rewrite. It is out of CX.

### CX-S3 - priority 2: access path selection

- The ladder stays the default selector, `f(shape, catalog)`.
- A managed plan may **pin** an arm per step (`ACCESS l INDEX x`) or
  **forbid** one (`ACCESS l NOT CABIN`), legality checked per CX-R2.
- §1.5's controller-created Cabins: CX-Q6.
- `ANALYZE` names, per step, whether its arm came from the ladder or from
  the plan.

### CX-S4 - priority 3: data distribution statistics, as a managed model

**Moved to its own order on W7** (`raft-marks-2026-11-18.md` §5):
`workorder-cy-managed-statistics.md`. CY builds the persisted relation
priors, the column statistics, the column-group statistics and the
management surface (`GATHER`, `LOCK`, `EXPORT`, `IMPORT`, history). What
stays here is the dependency: CX-S0's `fixed` default is only as good as
the statistics a first compile reads (CX-Q19), so **CY-S1 is CX-S0's
progress gate** (CY-Q0 (a), the operator's mark, `raft-marks-2026-11-18.md`
§6): `fixed` does not become the default until relation statistics survive
a restart. Which compiles read CY's model
is CY-R1: the search wherever CX-R1 runs it - `planned`'s every compile, a
`fixed` first compile, `ADVISE PLAN` - and `ANALYZE`'s estimates. The
sentence this section carried before W3, "never a plan", no longer follows
from CX-R1.

### CX-S5 - priority 4: join method selection

- The methods are the ones the tree has: the nested loop with a per-row
  probe (tuple key, index, Cabin), the walk, and the statement-local inner
  build.
- Under `written` the build sits on the written order's inner step, as
  before CB-S2; under `planned` and `fixed` its side is the search's, the
  captured plan carrying it under `fixed` (CX-R7).
- A managed plan sets `BUILD <rel> ON|OFF` per step. A plan that orders a
  relation inner picks its build side through `ORDER`, so CB's "smaller
  side" stays available as a plan rather than as an engine choice.
- §1.5's run-time decline: CX-Q8.
- A hoisted hash join is not built in CX (§0.1, CX-Q9).

### CX-S6 - priority 5: the plan cache

- `PlanMemo` keeps its one consumer, `planned` (CX-Q3, re-scoped by W3).
  Under `fixed` the captured map replaces it, and under `written` nothing
  is searched, so neither value reads the memo.
- A compiled-chain cache keyed `(fetch_id, oids, plan, variant,
  cache_built_at, catalog_version)` is built **only if** S6's first cell
  measures compile time as a material share of a repeated statement's
  latency at `cores = 1` and `cores = 2`. Its threshold is CX-Q10, set
  provisional until measured.
- Variant selection is evaluated per execution from the bindings. It is
  never cached across bindings.

### CX-S7 - priority 6: the join order search, as an advisor

- `ADVISE PLAN FOR <statement>` runs CB/CC's search with CY's model and
  PlanStats' facts. It returns the proposed plan as `CREATE PLAN` text,
  with its estimated cost ratio against **the plan in force** (an
  administrator's, a captured one, or the written order) and the
  statistics generation it read. Under `fixed` this is how a DBA or an
  agent learns that the statistics have moved past a captured plan (W4).
- `sys.plan_advice`: one row per advised shape (fetch id, proposal text,
  cost ratio, inputs generation, when). It is never read by a compile.
- Whether a background task advises the hottest fetch ids from
  `sys.statement_stats` without being asked is CX-Q11. CLA proposes the
  on-demand form first, because the background form needs a consumer the
  operator names.
- The loop W0 describes, closed: ASH and `sys.statement_stats` show a
  statement → `ADVISE PLAN` proposes → a DBA or an agent issues
  `CREATE PLAN` → `ANALYZE` and `sys.statement_stats` show the result.

## 4. Items

CX-Q0 and CX-Q1 were settled by W3-W5. CX-Q12..Q18 (W6) and CX-Q28 (W12)
are the operator's marks, (a). **CX-Q2..Q11 and CX-Q19..Q27 are adopted
as CLA proposed on the operator's standing go-ahead of 2026-11-18**
(W15), not marked item by item; CX-Q7 is withdrawn. A finding that
would change an adopted item is stated in the stage log where it is
taken; one that would change an operator's mark goes back to the
operator. The value names `planned`, `fixed` and `written` are CLA's
proposal; renaming them is the operator's.

| item | question | options | CLA proposes |
|---|---|---|---|
| CX-Q0 | W0 said *keep*; the tree needs *restore* (§1.1). Confirm that CB-S2's and CC-S3/S4's planned order stops being the default | (a) restore, as CX stated; (b) keep `planned` as the default and add managed plans over it | **Settled by W3-W5, neither option**: `planned` is kept as a value of `join_order`, and the default is `fixed`, which captures the first plan (CX-R1, CX-S0) |
| CX-Q1 | the `planned` setting after S0 | (a) delete the arm; (b) keep it as `ANALYZE`-only diagnostics | **Settled by W3**: `planned` stays a full value that chooses plans, the operator's opt-in to implicit change |
| CX-Q2 | the PlanStats steward between S0 and S7 | (a) keep it running; (b) stop it until S7 opens | **Adopted as proposed on the operator's standing go-ahead of 2026-11-18** (W15). (a), **now forced by W5**: a `fixed` first compile prices from it. Under `written` it could stop; CLA proposes it keeps running, so that switching to `planned` or `fixed` at run time (CX-R10) does not start from an empty snapshot |
| CX-Q3 | `PlanMemo` | (a) keep it for `planned` alone; (b) delete it and let `planned` search at every compile | **Adopted as proposed on the operator's standing go-ahead of 2026-11-18** (W15). (a): `planned`'s every compile is still its consumer (CC-S4 measured its hit); `fixed` and `written` never read it |
| CX-Q4 | an enabled plan whose arm becomes unservable | (a) refuse the DDL that would break it, RESTRICT, as `DROP TABLE` does for an assertion; (b) mark it `INVALID` and run the default plan, counted and visible; (c) refuse the statement | **Adopted as proposed on the operator's standing go-ahead of 2026-11-18** (W15). (a) for `DROP INDEX` and a Cabin drop, (b) for anything (a) cannot see. (c) turns a plan problem into an outage |
| CX-Q5 | the `WHEN` language | (a) `ARG(k) op constant`, `AND`, `IS NULL` only; (b) arithmetic over arguments too (`ARG(2) - ARG(1)`) | **Adopted as proposed on the operator's standing go-ahead of 2026-11-18** (W15). (b) limited to `+ -` over integers, checked overflow, because range width is the case that needs variants |
| CX-Q6 | controller-created Cabins (§1.5) | (a) a controller Cabin is ladder-eligible only through a managed plan that names it; (b) leave the ladder as it is | **Adopted as proposed on the operator's standing go-ahead of 2026-11-18** (W15). (a). Under (b), switching the controller on is an implicit plan change |
| CX-Q7 | where S4's model lives | (a) a logged `sys` relation; (b) memory, like PlanStats | **Withdrawn**: answered by CY-S1 (`workorder-cy-managed-statistics.md`): an unlogged `sys.statistics`, kept across a clean shutdown only (W14, W15) |
| CX-Q8 | the run-time build decline (§1.5) | (a) keep it as a resource limit, like `sort_max_rows`, visible in `ANALYZE`; (b) count it as a plan change and refuse instead | **Adopted as proposed on the operator's standing go-ahead of 2026-11-18** (W15). (a): it changes the locate mechanism, never the plan's shape, and a plan can say `BUILD OFF` |
| CX-Q9 | class (b) rewrites and the hoisted hash join | (a) not in CX; (b) as plan options in S2/S5 | **Adopted as proposed on the operator's standing go-ahead of 2026-11-18** (W15). (a) until a scenario statement is named as the consumer |
| CX-Q10 | S6's threshold for building the cache | a share of p50 latency, `[provisional]` | **Adopted as proposed on the operator's standing go-ahead of 2026-11-18** (W15). 5 % at `cores = 1`, provisional until S6's first cell |
| CX-Q11 | background advising | (a) on demand only; (b) a background task too | **Adopted as proposed on the operator's standing go-ahead of 2026-11-18** (W15). (a) first |
| CX-Q12 | what a capture holds (CX-R7) | (a) the order and the build side; each step's access arm stays the ladder's `f(shape, catalog)` given its predecessors; (b) the whole plan, arms included | **(a), the operator's mark** (W6). (a): W3's subject is statistics-driven change, and the ladder reads no statistic. Under (a) a `CREATE INDEX` can change a captured shape's arm, as DDL does under `written` - an explicit act. Under (b) a new index serves no captured shape until a DBA acts |
| CX-Q13 | one capture per shape or per binding | (a) one per shape, from its first compile's literals, serving every binding; (b) one per range-span band or per `arg_hash` | **(a), the operator's mark** (W6). (a): bindings that need different plans are an administrator's variants (`VARIANT ... WHEN ARG(k)`). Both options depend on history - under (a) the first binding compiled decides for every binding - and (a) keeps it to one visible row per shape, where (b) grows a set of plans on its own |
| CX-Q14 | a restart before a capture is persisted (CX-R8) | (a) accept it: the shape is captured again at its next compile, possibly differently, the window bounded by the persisting task's period; (b) persist at the compile, so a `SELECT` writes the log | **(a), the operator's mark** (W6). (a): (b) breaks CF's rule that no `SELECT` writes the log and makes a `READ ONLY` transaction write a record. The window and its count are in `sys.meta` |
| CX-Q15 | does a run-time `SET JOIN_ORDER` survive a restart | (a) no: memory-resident, the boot key is read again, as for `SET CABIN_OPTIMIZER` and `SET ASH`; (b) yes, persisted | **(a), the operator's mark** (W6). (a): a persisted run-time value is a second source of the setting beside the boot file |
| CX-Q16 | a captured plan that no longer compiles | (a) run the written order, mark the row `INVALID`, count it, never re-capture on its own; (b) re-capture | **(a), the operator's mark** (W6). (a): (b) is the engine changing a plan on its own. A DBA or an agent re-arms the capture with `DROP PLAN` (CX-Q17) |
| CX-Q17 | what re-arms a capture | (a) only `DROP PLAN` of the captured row, an explicit act; (b) also any DDL on a relation the shape names | **(a), the operator's mark** (W6). (a): W4's "the first plan is fixed". Under (b) a `CREATE INDEX` silently re-plans every shape over its relation from the statistics of that moment |
| CX-Q18 | switching values at run time | (a) `planned` to `fixed`: a shape with no captured row captures at its next compile from the current statistics, the memo dropped; `fixed` to `planned`: captured rows kept and not read; back to `fixed`: read again; (b) leaving `fixed` deletes the captured rows | **(a), the operator's mark** (W6). (a): the rows are the operator's record of what ran, and deleting them on a switch is a destructive side effect of a setting (cf. `cabin-controller.md` PO8: disabling leaves existing state untouched) |
| CX-Q19 | a `fixed` first compile before any statistic is published (S0, review A1) | (a) capture at once, as W5 reads: the data-free order is fixed; (b) defer the capture until the block's prices read a published fact, searching uncaptured until then; (c) seed the relations' priors before the listener opens, from persisted statistics (CY-S1), and capture at once | **Adopted as proposed on the operator's standing go-ahead of 2026-11-18** (W15). (c) with (b) as its fallback for a relation that has no persisted statistic yet. (a) fixes most of a freshly mounted instance's shapes on a plan chosen blind. (b) alone lets a plan move once, from blind to informed, which is the one change every reading of W4 would want |
| CX-Q20 | a planned block no capture key fits: more than 4 relations (`stats::PlanKey`, `plan_key.hpp:18`) or `fetch_id == 0` (`step_compiler.cpp:2408`) (review A2) | (a) widen the captured key to the planner's ceiling, 16 relations, so a greedy block is captured too; (b) run the written order for such a block under `fixed`; (c) search at every compile | **Adopted as proposed on the operator's standing go-ahead of 2026-11-18** (W15). (a): (c) is implicit change under the default, and (b) takes the planner away from exactly the blocks with the most orders. A `fetch_id == 0` statement runs the written order |
| CX-Q21 | the captured map's bound (review A4) | (a) a ceiling on captured shapes (`[provisional]` 65,536), past which a new shape runs the written order and is counted in `sys.meta`, nothing evicted; (b) no ceiling; (c) evict the least recently used | **Adopted as proposed on the operator's standing go-ahead of 2026-11-18** (W15). (a): (c) followed by a re-capture is an implicit change, and (b) lets a client that generates shapes grow a logged relation without limit |
| CX-Q22 | the map's cross-core protocol (review A5) | (a) one instance map for both origins, published as an immutable snapshot each core pins by generation (PlanStats' protocol, `plan-stats.md` PS5), captures staged in a per-core part and installed by the persisting task, first key wins; (b) a latched shared table probed at every planned compile | **Adopted as proposed on the operator's standing go-ahead of 2026-11-18** (W15). (a): the probe is one acquire load on a planned compile and nothing on a point read (CX-R6). Its cost: a shape compiled again before the task's next step is searched again and may run a different order until the install, which is CX-Q14 (a)'s window inside one run. CX-R6's "per-core read-only map" becomes this one snapshot |
| CX-Q23 | does persisting captured rows move the schema version word (review A12) | (a) no: a `sys.plans` write bumps a plan generation of its own, which the map's snapshot carries; (b) yes, as every catalog write does | **Adopted as proposed on the operator's standing go-ahead of 2026-11-18** (W15). (a): under (b) every capture batch makes every core revalidate its catalog memo and empties `PlanMemo`, whose key holds `catalog_version` (`step_compiler.cpp:2462`) |
| CX-Q24 | plans over a dropped or renamed relation (review A6) | (a) `DROP TABLE` deletes the captured rows naming its oid in its own transaction and is RESTRICTed by an enabled administrator's plan, as by an assertion; a rename orphans a captured row (its fetch id hashes names, `fingerprint.hpp:44-48`), which stays until `DROP PLAN`; (b) RESTRICT for both origins | **Adopted as proposed on the operator's standing go-ahead of 2026-11-18** (W15). (a): (b) refuses every `DROP TABLE` of a relation that was ever joined under the default |
| CX-Q25 | what a captured row records of the statistics it read (review A10) | (a) one bit, informed or not, and the wall-clock time of the capture; (b) the generation number | **Adopted as proposed on the operator's standing go-ahead of 2026-11-18** (W15). (a): the generation is an in-memory counter a restart resets, so a persisted one compares with nothing. With CY's persisted model the row can instead name the model versions it read (CY-R6), which S7's "the statistics have moved past this plan" needs |
| CX-Q26 | a captured order that fails to compile for one binding only (review A7; reachability not verified) | (a) that compile runs the written order and the row stays; `INVALID` (CX-Q16) is for a failure the catalog causes; (b) any failure marks the row `INVALID` | **Adopted as proposed on the operator's standing go-ahead of 2026-11-18** (W15). (a): under (b) one binding takes the captured plan away from every binding |
| CX-Q27 | a boot change of `join_build_max_rows` or `sort_max_rows` after a capture (review A13; `PlanMemo` keys on both, PS6) | (a) captures stay: a limit is a resource bound checked at run time (CX-Q8 (a)), and `ADVISE PLAN` shows the difference; (b) a changed limit re-arms every capture | **Adopted as proposed on the operator's standing go-ahead of 2026-11-18** (W15). (a): (b) is a re-plan of the instance by a configuration edit |
| CX-Q28 | how a user fixes a plan for an uncaptured shape (CX-R11, W10) | (a) both: `CREATE PLAN` written by hand, and `CREATE PLAN <name> FOR <statement> AS SEARCHED`, which runs the search once with the statistics of that moment, shows the order, and stores it as an administrator's plan; (b) `CREATE PLAN` by hand only | **(a), the operator's mark** (W12). (a): W10 asks that the user can always fix a plan, and writing `ORDER (...)` by hand for a shape the engine is already planning is work the search has done. The stored row is an administrator's (`origin = admin`), not a captured one, so nothing re-arms or defers it |

## 5. `[quiet-wrong]` risks

- **A plan, an administrator's or a captured one, applied to the wrong
  statement**: closed by CX-R3 (and CX-R7, which uses its key). Matching
  compares every oid exactly. A fetch id hashes names, not oids
  (`include/kds/parser/fingerprint.hpp:44-48`), so a renamed and
  recreated table must not inherit a plan. The oid vector is what makes
  that miss.
- **A forced arm that cannot serve the step**: closed by CX-R2. Legality
  is checked at every compile, because a catalog change after `CREATE
  PLAN` can make a legal arm illegal.
- **A variant rule that is not total**: closed by CX-R4's mandatory
  `ELSE` and NULL-to-`ELSE`.
- **An order that changes results under `LIMIT` without `ORDER BY`**: not a
  risk to correctness, because I11 makes that prefix no contract. It is a
  visible behaviour change between plans, though, so the client manual
  states it beside `CREATE PLAN`.
- **Two cores running two captured plans for one shape**: closed by
  CX-R7's first-install-wins. Even if it happened, every order of an inner
  block returns the same rows (CB-S2's premise), so the risk is to "no
  implicit change", not to results.
- **A `SELECT` that writes**: closed by CX-R8. A capture installs in memory
  and is persisted by a `system` task; a cell asserts that a `fixed` first
  compile under `BEGIN READ ONLY` appends no record.

## 6. Measurement

AS-E epoch, `build-release`, results named by `git describe --tags`.

- **S0's close**: scenarios 1-4 on the opening commit (A, `planned`)
  against the S0 build under `fixed` (B) and under `planned` (B'). Gates:
  B' equals A within noise (the `planned` value unchanged); B equals A
  within noise **for shapes captured informed** (CX-Q19), each cell's
  `informed=` asserted;
  the captured-map probe costs nothing on a point read (CX-R6). Then the
  one thing `fixed` gives up, measured once: a shape captured from an empty
  snapshot (`informed=0`) against the same shape under `planned` once the
  statistics are published, listed with p50 and p99. This is the price of
  W4's "the first plan is fixed", recorded and not optimised away.
- **S1's close**: the same statements with an administrator's plan that
  restores A's order where `fixed` captured a worse one, against A. The
  target is equal within noise. The plan-lookup overhead is measured on a
  point read (must be zero, CX-R6) and on a two-relation join; a restart
  shows every persisted capture back in force with no search.
- **S6's first cell**: the compile's share of a repeated statement's
  latency, which decides CX-Q10.

## 7. Contracts amended (when the stages land)

- `docs/spec/parser-v2.md` I12 - rewritten around CX-R1's three sources
  and `join_order`'s three values (S0), with managed plans (S1).
- `docs/spec/plan-stats.md` PS6 (re-scoped to `planned`, S0), PS9, PS12.
- `src/server/expeditor.cpp`'s `join_order` refusal text, `kds.conf.sample`,
  `manual/server/` - the three values and the `fixed` default (S0);
  `docs/spec/protocol.md` and `manual/sql/` - `SET JOIN_ORDER` (S0).
- `docs/spec/join-inner-build.md` - the build side (S0, S5).
- `docs/spec/index.md` IX9 - "must not compile differently as the data
  changes" restored for `fixed` (after a shape's first compile) and
  `written`, and stated as the operator's choice under `planned`, which CC
  re-scoped it for.
- `docs/spec/cabin.md` §4e - under CX-Q6 (a).
- `docs/spec/sys-views.md` - `sys.plans`, `sys.plan_advice`, the new
  `sys.statement_stats` columns.
- `manual/` - `CREATE PLAN`, `ADVISE PLAN`, `SET JOIN_ORDER`.

## 8. Stage log

**CX opened on W14** (`raft-marks-2026-11-18.md` §16); its stages run
after CY-S1 (CY-Q0). Amended 2026-11-18 on
W3-W5 (`raft-marks-2026-11-18.md` §3), documents only. CX-Q12..Q18 marked (a) on W6 (§4), documents only.

**The review of the amendment** (`critics-developer`, read at `d4bda61f`,
reported after W6's push). Taken: sixteen of its seventeen wording corrections (the
overlong line it named in the header is not reflowed); its design findings
A1-A8, A10 and A12, and the second half of A13, as CX-Q19..Q27 and as
CX-R7's `?` and minted-view sentences; its duplicate §5 bullet, folded; A9 as CX-Q13's corrected reason; A11 as CX-S4's
move to CY. Its largest finding is A1: under `fixed` a shape first compiled
before the steward's first publish is fixed on the data-free order, which
made S0's "costs nothing after warm-up" and its close gate false; both are
restated. Not taken: folding the administrator's lookup and the captured
map into one structure is left to CX-Q22 rather than decided in prose; the
repetition it found between CX-R8, S0 and CX-Q14, in the four statements
that a capture is not adoption of advice, and in the steward's two
mentions is kept, each being a rule, a stage's fact or the operator's
item. A second review, of `fb65d7bd`, found A10 unapplied in S7 (the
"statistics generation" an advice names is an in-run value and stays, the
model being CY's) and three stale lines after S4's move, each corrected. Its A13 note that
controller-made Cabins still reach the ladder under `fixed` is CX-Q6,
unmarked before the amendment and still.
