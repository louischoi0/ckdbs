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
  no literal. **Under a minted view (a DDL open anywhere in the instance)
  no capture is made, and a held one is still run** (amended at S0's
  review, §8: as first written none was read either): an order priced
  against a catalog state that may roll back is not fixed, and a shape
  with no captured order is searched uncaptured there; a captured order
  was fixed outside any view for these exact oids and goes through the
  ladder under the view, so it is read - otherwise one open DDL would let
  every captured shape follow the statistics for as long as it stayed
  open. **A captured order is declined for one statement, never forced**,
  where it does not compile for it (CX-Q26) or adds a sort its `LIMIT`
  does not bound (CC-Q10, asked of every binding, a `LIMIT` being a
  literal outside the key): that statement runs its written order and
  the capture stays.
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
| CX-Q29 | who may run `ADVISE PLAN` (S7) | (a) `admin`, as the plan DDL; (b) `readonly`: it writes nothing durable | **Adopted on the operator's standing go-ahead of 2026-11-18 (go-ahead-achieving-milestone)**, CLA's proposal: (a). Its row lands in an instance table every admin reads and its search runs on the statement's core; `RequiredRole` already classes an unlisted verb `admin`, so (a) adds no line to the role table |
| CX-Q30 | where `sys.plan_advice` lives (S7) | (a) memory: one table for the instance, emptied by a restart, a newer advice for a shape replacing the older, bounded at 1,024 shapes `[provisional]`, as CX-Q21's ceiling is, with the oldest dropped past it; (b) a logged `sys` relation | **Adopted on the operator's standing go-ahead of 2026-11-18 (go-ahead-achieving-milestone)**, CLA's proposal: (a). Advice is never read by a compile (S7), and it is the reading of a moment's statistics, which are themselves unlogged (W14); (b) would be the second logged catalog relation of this order for state nothing depends on, and a superblock bump |
| CX-Q31 | what of the statement an advice keeps (S7) | (a) the view holds the shape text, literals as `?`; the reply alone carries the `CREATE PLAN` text with the example's literals, issuable as it stands; (b) the view holds the literals too | **Adopted on the operator's standing go-ahead of 2026-11-18 (go-ahead-achieving-milestone)**, CLA's proposal: (a), `sys.plans`' rule that no row holds a client's value (`plan_catalog.hpp`) |
| CX-Q32 | "the plan in force" an advice is priced against (S7) | (a) the order and pins this statement's next compile would run: an enabled administrator's plan's variant chosen for its literals; else under `fixed` the shape's captured order; else under `planned` the search's (the ratio 1); else the written order - compiled through the ladder and priced by the search's own cost inputs, nothing captured, memoised or counted by the advice; (b) the written order always | **Adopted on the operator's standing go-ahead of 2026-11-18 (go-ahead-achieving-milestone)**, CLA's proposal: (a), which is S7's "against the plan in force (an administrator's, a captured one, or the written order)"; (b) would tell a DBA nothing under `fixed`, the default, where the question is whether the statistics have moved past a captured order (W4) |
| CX-Q33 | an advice for a statement the search does not order (S7) | (a) a statement over one relation is refused `Unsupported`, as `AS SEARCHED` is; a chain of tuple key descents and a subquery predicate are advised their written order with the ratio 1 and why, never searched - the plan text given only where `CREATE PLAN` takes the block (a joined `SELECT *` never reaches the advice: the parser refuses it); (b) refuse every one | **Adopted on the operator's standing go-ahead of 2026-11-18 (go-ahead-achieving-milestone)**, CLA's proposal: (a): a one-relation statement has no order to propose; the others have one, and saying the engine would not search them is the answer |
| CX-Q34 | a mount meeting a `sys.plans` it cannot scan or delete from (S1a; `known-gaps.md`) | (a) refuse the mount, as for every logged catalog relation (today); (b) mount with the plans discarded, counted | **Adopted on the operator's standing go-ahead of 2026-11-18 (go-ahead-achieving-milestone)**, CLA's proposal: (a), as built. The relation holds administrators' plans as well as captures, and (b) would drop them with no word - every planned shape moving at the next compile, the implicit change W0 forbids - where a capture alone would cost only a recapture. A row it can scan and cannot read stays skipped and shown `unreadable` |
| CX-Q35 | what `BUILD <rel> ON` means (S5; the order gives only the clause) | (a) the step held to the build - every keyed class stands aside and the pin is met only by a step carrying the build annotation; (b) a check alone: met where the ladder already built | **Adopted on the operator's standing go-ahead of 2026-11-18 (go-ahead-achieving-milestone)**, CLA's proposal: (a), as built at S5 (§8). (b) adds nothing - the ladder builds where it can - and S5's subject is choosing the join method; (a) stays inside CX-R2, the build being the ladder's own last rank for the shape |
| CX-Q36 | the cabin controller under CX-Q6 (a) (S3b) | (a) keep CX-Q6 (a) as built: a controller-made Cabin serves only through a plan that names it, so the controller accelerates nothing until one does; (b) let the ladder take a controller Cabin again under `join_order = planned` only | **Adopted on the operator's standing go-ahead of 2026-11-18 (go-ahead-achieving-milestone)**, CLA's proposal: (a), as built at S3b (§8). CX-Q6 itself was adopted on W15; this records that its consequence was weighed and kept. (b) is the operator's to choose later: `planned` is already the opt-in to plans moving on their own (W3), so a controller Cabin there would be consistent - but it is a second change of what `planned` means, and nothing in this order asks for it. S7's `ADVISE PLAN` is where a plan naming a controller Cabin could later be proposed |
| CX-Q37 | what S6's first cell decides (CX-Q10's line, measured) | (a) **build no compiled-chain cache**: CX-Q10's "only if" is a necessary condition, and the server's own `exec::Compile` - the ceiling on what a fetch-id-keyed cache could remove, the lex, parse and fingerprint excluded - crosses 5 % of the client's p50 in the plan cells alone (a two-variant plan, 5.7 % at `cores = 1` and 6.3 % at `cores = 2`; a one-relation plan, 5.3 % at `cores = 2` only) and stays at 2.8 % to 4.1 % in every plain shape; (b) build the cache for statements an administrator's plan governs; (c) build it for every shape, the line read against the server's own time, which every cell crosses (5.3 % to 15.2 %) | **Adopted on the operator's standing go-ahead of 2026-11-18 (go-ahead-achieving-milestone)**, CLA's proposal: (a), revised after the review of S6 against the spiked server's numbers (`bench/v3.4.12/results-cx-s6-compile-share-v3.4.12-112-g9712f5f0.md` §8; §7's in-process figures ran hot and are superseded). Why not (b): what makes the plan cells cross is the plan's own work - a multi-variant plan lexes the statement again to choose its variant, which a cache keyed by the variant still pays, and a variant other than the written order compiles twice - so the margin a cache could win there is about a point over a line drawn against a Python client, before the cache's own lookup, rebinding and invalidation; and a chain cached without its literals and bound again per statement is the one structure in this order that could answer wrongly if a literal lands in the wrong slot. Why not (c): CX-Q10 drew the line against the statement's p50, and re-drawing it is the operator's choice. (b) and (c) stay open for the operator; the plan cells' extra compile is in `known-gaps.md` |

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

### CX-S0 - built 2026-11-18; its §6 measurement not run

On worktree `cx-plan-change-option`. `join_order` is `planned | fixed |
written` at boot and by `SET JOIN_ORDER`; `Expeditor::Config`'s default is
`fixed`; `exec::CapturedPlans` is the instance's table, and
`CompileBlock` asks it after the tuple key chain's skip and before the
statistics are pinned. `parser-v2.md` I12, `plan-stats.md` PS6, PS9 and
PS12, `index.md` IX9, `join-inner-build.md` §2, `sys-views.md`,
`protocol.md` §14's role sentence, `client-manual.md`, `kds.conf.sample`
and `manual/` state it. The four contract suites each gained a cell under
`fixed` with their shapes captured, and `tests/captured_plans_test.cpp`
carries the capture's own cells.

**Facts of the stage.**

- **Memory only.** Until S1 persists a capture, a restart captures every
  shape again (CX-R8).
- **The §6 measurement for S0's close was not run.** Overhead is measured
  at the milestone's close (CLAUDE.md, Session Workflow step 3). S0 is
  built and its suite is green; it is not closed on a measurement.
- **A dispatcher with no instance behind it runs `planned`**, as before CX:
  every suite's rig builds one bare, and moving them to `fixed` would have
  changed what several hundred cells run without one of them asking for
  it. The server's default is the `Config`'s. The simulation harness's
  instance is such a dispatcher, so the corpus runs `planned`.
- `StatementLimits::plan_join_order` is deleted: the value is the
  instance's atomic, handed with the table through `OptimizerSurface`, so
  a boot value and a run-time value are one word and not two.

**Where the build differs from an adopted item** (§4: "a finding that
would change an adopted item is stated in the stage log where it is
taken").

- **CX-Q22: (b) is built, not the adopted (a).** The table is 64 latched
  partitions that every core's compile probes, not a published snapshot
  with per-core staging. Under (a) a shape compiled on two cores before
  the next install runs two orders until it, and CX-R7 says no shape runs
  two captured plans; (b) gives first-install-wins at the compile. Its
  cost is one partition latch on a searched block's compile, and nothing
  on a point read or a tuple key chain, which leave before it (CX-R6).
  S1's persisting task reads the table; it does not install into it.
- **CX-Q19: a capture waits for every relation's prior, not for "a
  published fact".** (b) as adopted would capture a three-relation block
  once one of its relations had a prior, two still priced at the constant.
  The build captures only when every relation of the block has one. (c) is
  CY-S1's seed at the mount, already built; a relation it does not cover
  is seeded by the steward within its next steps after the first deferred
  run names it.
- **`informed=0|1` is not printed** (S0's `ANALYZE` bullet, CX-Q25). With
  the rule above no uninformed capture exists, so the bit would be a
  constant. S1's row can still carry the time of the capture.
- **CX-Q3 kept as adopted, and its cost stated**: a deferred shape is
  searched at every compile without the memo. `known-gaps.md` (Captured
  plans) carries it; not measured.
- **CX-R11's `plan_source` column of `sys.statement_stats` is not built in
  S0.** `ANALYZE` names a statement's source and `SHOW META` counts the
  uncaptured blocks by reason; the per-shape column goes with S1's
  `plan_name` and `variant`, the stage that gives a reader something to do
  with it.
- **CX-Q20's key is the capture's own**, 16 relations wide
  (`exec::CapturedKey`), not `stats::PlanKey`, which stays at four for the
  posteriors.
- **CX-Q18 (a)'s "the memo dropped"** is built as the memo left where it
  is and not read under `fixed`. Its key holds the statistics generation
  and both catalog stamps, so an entry made before a switch is still the
  search's exact answer for its key when `planned` comes back.
- **CX-R7 amended**: a captured order is read under a minted view (the
  review's F1, below).

**The review** (`critics-developer`, of `63ce2c93`; its fixes and the ones
taken from its findings are in the commit after it).

- **Fixed by the review - a captured order could refuse a statement that
  `planned` answers.** `LIMIT` is a literal, so it is outside the key. A
  reordered chain that adds a sort is admitted by the search only while
  `LIMIT` bounds the sort to a top-N heap (CC-Q10); captured under `LIMIT
  5`, the same order under `LIMIT 100` was an unbounded sort and met
  `sort_max_rows` where the written order streams. Reproduced before the
  fix with `sort_max_rows = 10`. CC-Q10's rule now has one home
  (`exec::AddedSortCannotRefuse`), the search and a captured order's run
  both ask it, and a binding it turns away runs as written
  (`capture=declined`), the capture kept - CX-Q26 (a)'s answer.
- **Fixed by the review**: `captured_served` counted a declined compile
  and not one that lost the install to another core; it now counts every
  compile that found an order in force. A comment said a minted view
  means a DDL in this transaction; it is any DDL open in the instance.
- **Taken - F1, a captured order is read under a minted view.** As built
  first, one session holding a DDL open made every captured shape on
  every core search against the current statistics for as long as it
  stayed open: implicit change under `fixed`, and up to 24 compiles a
  statement. Now a held order runs there and only the capture is withheld
  (counted `capture_deferred`). CX-R7's sentence is amended to match.
- **Taken**: the two reasons renamed to say what they are, one name each
  in `ANALYZE`, `SHOW META` and the code (`over_ceiling`, as CX-R11 has
  it, for `refused`; `declined` for `uncompiled`, which no longer
  described a chain that compiles and is not run); the optional written
  chain and its recompile in `CompileBlock` replaced by the chain that is
  always at hand; `set_plan_join_order(bool)` deleted, its twelve call
  sites naming the value; a cell that a configured `join_order` reaches
  core 0 and a peer as one word and one table (F2); a cell that `fixed`
  neither reads nor fills the memo; the table and the word declared in
  `rules.md` §3 (F4).
- **Not taken**: one loop building both `stats::PlanKey` and
  `CapturedKey` - they differ in width, in type and in the condition
  under which each exists, and the merged loop is no shorter. A latch kind
  of its own for the table (F4's second half): `kStatsRing` is what
  `PlanStats` uses for the same shape of hold, and a new kind adds a
  contention tally to `sys.meta` for a latch not yet seen to wait.
- **Left as gaps** (`known-gaps.md`, Captured plans): a capture made by a
  compile `ResolveNames` then discards (F3); the paths no cell reaches.

### CX-S1 split; CX-S1a - built 2026-11-18

S1 is taken in two parts, as CY-S1 was. **S1a** is the plan as a catalog
row and nothing a person writes: `sys.plans`, the captured orders
persisted and loaded, `DROP PLAN`, the view. **S1b** is the
administrator's plan: `CREATE PLAN ... FOR ... VARIANT ...`, `AS
SEARCHED`, `ALTER PLAN`, the `WHEN` rule, source 1 of CX-R1 in the
compile, `ANALYZE`'s `plan=` and `sys.statement_stats`' columns. `ACCESS`
is S3's and `BUILD` S5's, as §3 has them.

**S1a, as built** on worktree `cx-plan-change-option`:

- **`sys.plans`** is the fourth declaration relation (oid 161, root page
  18, **superblock 37**, which refuses 36): `aux` is the fetch id, `flags`
  bit 0 says captured, and the text is `v1 fp=... oids=... order=... at=...` and,
  after a newline, the capturing statement's **shape text** - the statement
  with every literal written `?` (`parser::ShapeText`, new), so no row
  holds a client's value. Logged at `kNoTxnId`, as every declaration is.
- **The persisting task** (`exec::PersistCapturedPlans`, CX-R8): an install
  queues its capture; core 0's `system` group drains the queue once a
  second, up to 64 rows a step, and the shutdown tail drains what is left.
  A row's name is `captured_<fetch_id>_<row id>` - the row id because one
  fetch id is two shapes when a statement binds another namespace's
  relations or a relation created again.
- **A mount** (`exec::LoadCapturedPlans`) installs every readable row
  before the listener binds, after recovery and the var-heap sweep; a row
  naming a relation that is gone is removed there, and one this build
  cannot read is left and counted.
- **`DROP PLAN <name>`**, admin, autocommit only, removes the row and
  forgets the shape (CX-Q17). **`SHOW PLANS` / `sys.plans`** lists the rows.
- `SHOW META` gains `captured_pending`, `captured_persisted`,
  `captured_loaded`.

**Where S1a differs from an adopted item.**

- **CX-Q24 (a)**: a dropped relation's captured rows are not deleted "in
  its own transaction". A declaration relation's delete is physical and
  outside every transaction, so doing it inside the drop would either
  survive the drop's rollback or refuse `DROP TABLE` in a transaction, as
  a Transition does. Built instead: where the drop **commits**
  (`EndDdlScopeById`), the shapes naming the oid are forgotten in the
  table and their oid is owed to the persisting task, which removes the
  rows in its next step; a mount removes any a crash left. A drop that
  rolls back forgets nothing. The administrator's-plan RESTRICT half of
  the item is S1b's.
- **CX-Q16's `INVALID`** is not built in S1a: a row has no state column to
  move, and S0's review settled that a captured order which does not fit a
  statement is declined for that statement and kept.
- **`plan_source` / `plan_name` / `variant`** in `sys.statement_stats` and
  `ANALYZE`'s `plan=` line are S1b's, where a plan first has a name a
  statement can run under.

**Found on the way, outside CX.** Bootstrap's fixed catalog roots are now
eight consecutive pages (11..18). A writeback run pins up to eight frames
for its copy, which is the whole of the Debug build's per-operation pin
ceiling, so a full run under any caller that holds a pin of its own - the
simulation harness's sync inside a statement did - aborted. The run's pins
are now an allowance of their own (`device_page_store.cpp`, `page.md` §3),
and the abort prints frames. A release build never checked.

**The review of S1a** (`critics-developer`, of `4f62dd2a`; its fix and
the ones taken from its findings are in the commit after it).

- **Fixed by the review - a failed write could put back a capture whose
  relation had just been dropped.** `Requeue` asked the table and then
  queued, under two separate holds; a `ForgetRelation` passing between
  them erased the shape, found the queue not yet holding it, and owed its
  oid to a step that ran before the capture's row was written - a row for
  a dropped relation that only the next mount removed. It now asks and
  queues under one hold of the partition, as an install does.
- **Taken - the persisting step was quadratic.** Every declaration insert
  scans its whole relation for the name, resolving each row's spill, and a
  step makes up to 64; every removal scanned again by name. A captured
  row's name holds its own id, so its insert skips the check
  (`DeclarationName::kUniqueByConstruction`), and a step's or a mount's
  removals are one walk (`DeleteDeclarationsById`). Not measured; the
  reviewer's estimate at the 65,536-shape ceiling was of the order of 10^9
  spill fetches on core 0's reactor.
- **Taken - one row a shape at a mount.** A row whose relation could not
  be opened for a reason other than its absence is neither loaded nor
  removed; its shape is captured again and a second row written. The next
  mount then loaded whichever it met first, so the order in force could
  change across a restart. A mount now removes a row for a key it has
  already loaded.
- **Taken**: the row records the fingerprint version, and a mount removes
  a row of another one, which could match no statement and would only
  count against the ceiling; `ForgetRelation` owes the task nothing where
  no shape named the relation, so a drop no plan names costs no scan; the
  shape text is made before any latch and cut to what a row keeps;
  `Expeditor::PersistCapturedPlans` answers a `Status` and logs a run of
  failures once; the step has its own tick constant; `DecodePlanText`
  writes nothing on a refused text and refuses a trailing space and a time
  past `int64`; `CapturedKey::Names` replaces two copies; cells for the
  value limit through an insert, a drop committed inside a transaction,
  the re-capture after a crash, a comment and unreadable text in the shape
  text; the manuals say a written row survives a crash only once the log
  has next been synced.
- **Not taken**: a cell of its own for the writeback run's pin allowance -
  four simulation cells abort without it, which is how it was found. The
  two-line branch in `HandleDropTable` that forgets at once for a drop
  under no transaction stays: only a hand-built dispatcher reaches it, and
  without it those rigs could not show the forget at all.
- **Left for the operator**: a mount refuses on a `sys.plans` it cannot
  scan, as on any logged catalog relation, though what is lost would cost
  a recapture (`known-gaps.md`).

### CX-S1b - built 2026-11-18; its §6 measurement not run

The administrator's plan, on worktree `cx-plan-change-option`.

**As built.**

- **The DDL** (`exec/plan_ddl.hpp`): `CREATE PLAN <name> FOR <select>
  VARIANT <v> WHEN <rule> ORDER (...) ... VARIANT <w> ELSE ORDER (...)`,
  `CREATE PLAN <name> FOR <select> AS SEARCHED`, `ALTER PLAN <name> ENABLE
  | DISABLE`, `ALTER PLAN <name> REPLACE VARIANT <v> (WHEN <rule> | ELSE)
  ORDER (...)`, and S1a's `DROP PLAN`. Admin, autocommit only, answered
  once durable. No word is reserved: the `<select>` ends at the first
  `VARIANT <identifier> WHEN|ELSE` outside parentheses or at a closing `AS
  SEARCHED`, neither of which a statement a plan can be made for holds.
- **The rule** (`exec/plan_rule.hpp`, CX-R4): comparisons joined by `AND`
  over sums of `ARG(k)` and integers, checked for overflow; total - a
  comparison that cannot be decided is false. `ARG(k)` is the statement's
  k-th value position (`parser::ArgumentsOf`, new): every integer, numeric
  and string literal in token order, a `LIMIT`'s among them.
- **The table** (`exec/admin_plans.hpp`): one for the instance, 16 latched
  partitions, holding every administrator's plan whose relations exist and
  serving the enabled ones. `CompileBlock` asks it first, under every
  `join_order` (CX-R1's source 1); under `written` the dispatcher hands the
  compile its options only while a plan is enabled, so an instance with
  none compiles as it did.
- **The row** (`exec/plan_catalog.hpp`): `flags` bit 0 clear, bit 1
  `DISABLED`; text `v1 fp= oids= bindings= args= at= variants=`, one line a
  variant, then the shape text. A mount loads both origins (`LoadPlans`).
- **`DROP TABLE`** is RESTRICTed by an enabled plan naming the relation
  (CX-Q24 (a)); a committed drop forgets a disabled one in the table and
  leaves its row.
- **Seen**: `ANALYZE`'s `planned order=plan plan=<name>/<variant>`;
  `sys.plans`' `variants` column and its `disabled` and `invalid` states;
  `sys.statement_stats`' `plan_source`, `plan_name` and `variant`; `SHOW
  META`'s `admin_plans`, `admin_plans_enabled`, `plan_served`,
  `plan_declined`.
- `tests/admin_plans_test.cpp` carries the stage's cells; `parser-v2.md`
  I12, `sys-views.md`, `drop-table.md` DT3, `rules.md` §3, `client-manual.md`
  and `manual/sql/sql.md` state it.

**Facts of the stage.**

- **The §6 measurement for S1's close was not run.** Overhead is measured
  at the milestone's close. What S1b adds to a statement is named in
  `known-gaps.md` (Captured plans) and not measured: one relaxed load a
  top-level join block's compile under `planned` and `fixed` - under
  `written` the load is the dispatcher's, on every `SELECT`, a
  single-relation one included - a probe while any plan is enabled, and a
  second lex of the statement for a plan with more than one variant.
- **`ACCESS` and `BUILD` clauses are refused `NotImplemented`** at their
  word; they are S3's and S5's.

**Where the build differs from an adopted item.**

- **CX-Q5: `IS NULL` is not built, and is refused `Unsupported`.** NULL is
  shape to the fingerprint (`fingerprint.hpp`: "NULL is shape"), and a
  `C_BIND` of NULL substitutes the word into the text, so a statement with
  a NULL in a value position has another fetch id and matches another
  plan. No argument of a matching statement is ever NULL, and CX-R4's "a
  NULL argument matches no comparison and reaches `ELSE`" has no case. A
  spelling that can never hold is not accepted.
- **CX-Q5: a string compares, by `=` and `!=` only.** (b) as adopted is
  arithmetic "over integers"; (a)'s `ARG(k) op constant` did not say what
  a constant is. A skewed categorical value (`status = 'CLOSED'`) is the
  other case that needs variants, so a string constant is admitted for
  equality, by its bytes. Ordering strings is `NotImplemented`: a numeric
  with a point is a string here and its byte order is not its order.
  `OR` is `NotImplemented`; another variant is a rule's `OR`.
- **CX-R6: a tuple key chain is probed while a plan is enabled.** As
  written, "a point read or a tuple key chain leaves the compile before
  any plan lookup". A point read still does - one relation never reaches
  the table. A block of tuple key descents is asked, because W4 makes an
  administrator's plan "always possible" and a plan created for such a
  shape and then never applied would enforce nothing of what was written.
  The cost is a relaxed load while no plan is enabled and one partition
  latch while one is.
- **CX-S1's grammar: by example, with literals.** The sketch wrote the
  statement with `?`. The native grammar reads no `?` and validation
  needs "representative bindings", so `CREATE PLAN` takes the statement
  with literals and refuses a `?` `NotImplemented`, as `WAYSTONE ON` does
  (CF-O-Q11 (a)). The row stores the shape text, so no literal is kept.
- **CX-Q4 (b), CX-Q16: `INVALID` is derived, and a decline marks
  nothing.** A plan reads `invalid` where a relation it names is gone or
  its fetch id is another fingerprint version's - facts read at the
  moment, never a stored bit - and then serves nothing, refuses `ALTER
  PLAN` and stays until `DROP PLAN`. A variant that does not fit one
  statement is declined for it, CX-Q26 (a)'s answer for a captured order:
  marking the row would take the plan from every binding for one
  binding's sake.
- **A declined variant runs the written order**, not "the default plan"
  (CX-Q4 (b)): the captured order's decline does the same, and falling to
  the search would let a plan's failure start a capture.
- **`REPLACE VARIANT` is not compiled** - the row holds no literal to
  compile with. It is checked as a permutation of the plan's bindings and
  against the argument count (`known-gaps.md`).
- **The row carries no creator and no reference list** (CX-S1's `sys.plans`
  bullet): a session has a role and no name, and a variant names no index
  or Cabin until S3. It carries the bindings and the argument count
  instead, which `REPLACE VARIANT` reads.
- **CX-S1's `ANALYZE` spellings.** `plan=<name>/<variant>` is built. The
  reasons a shape has no plan keep the names S0 gave them on the `planned`
  line (`order=captured`, `capture=deferred|unkeyed|over_ceiling|
  declined`); `sys.statement_stats`' `plan_source` carries CX-R11's names.
- **Plan DDL takes one claim and never waits** (`known-gaps.md`): CX-Q22
  left the map's protocol open for the administrator's side, and "one
  plan a shape" and an `ALTER`'s insert-then-remove need one writer at a
  time. And `CREATE PLAN` and `ALTER PLAN` hold their relations' `IS`
  until the plan is installed, refusing `TxnConflict` where a `DROP TABLE`
  holds the `X`, so no plan is installed over a relation whose drop then
  commits.

**The review of S1b** (`critics-developer`, of `4f12b44d`; its fix and the
ones taken from its findings are in the commit after it). It found no way
for a plan's order to reach a statement it does not belong to or to change
a result.

- **Fixed by the review - a streamed `SELECT` lost its plan in
  `sys.statement_stats`.** On the server's path a `SELECT` whose first step
  walks pages is finished after its head returns, and the walk's end
  rebuilt the outcome without `plan_source` and `plan_label`: every such
  statement recorded NULL, or what an earlier one left. Every cell of the
  stage went through `Dispatch`, which runs the walk inline. The walk now
  keeps the two as it keeps the statement's id, and a cell drives
  `DispatchAsync`.
- **Taken - a plan-served chain folded no join statistics.** The plan's
  branch named the chain's relations and not the key its counts fold
  under, so a shape an administrator's plan served gathered no posterior
  and a later `DROP PLAN` or `AS SEARCHED` priced from priors alone
  (CX-Q2). The key is set there as the captured path sets it; a cell.
- **Taken - `ALTER PLAN` could answer "invalid" for a drop that might
  roll back.** Validity was asked before the relations' `IS`: beside an
  open `DROP TABLE` the catalog can already show the relation gone, and
  the refusal was `Unsupported`, "DROP PLAN it", where it is the
  retryable `TxnConflict`. The hold is asked first.
- **Taken - "every `IS` was granted" was counted across two compiles.**
  `ResolveNames` can compile twice and bind a name to two relations, and
  a grant on one counted for the other. `HoldPlanRelations` takes a borrow
  of its own on the key's oids, for `CREATE PLAN` and `ALTER PLAN` both,
  and `CREATE PLAN` asks the catalog once more under it.
- **Taken - only `NotFound` says a plan's relation is gone**
  (`PlanRowIsValid`), as for a captured row: any other failure at a mount
  had left a valid plan out of the table, where it neither served nor
  RESTRICTed, and let a second plan be made for its shape. A mount
  installs in id order, so no hash order decides between two rows.
- **Taken**: `plan_source` is recorded for the statement's own `SELECT`
  only, not for a subquery an `UPDATE`'s `SET` runs; a shape's source and
  label are replaced as a pair; `sys-views.md` §6.1 said NULL means "no
  join block" where it is every block the planner does not order; a
  rename's effect on a plan is stated (`parser-v2.md` I12, `known-gaps.md`);
  this log's "one relaxed load" sentence is corrected for `written`.
- **Taken, as cuts**: the variant checks `CREATE PLAN` and `ALTER PLAN`
  each wrote out are one `exec::ResolvePlanVariant`; three case-folding
  helpers are one `exec::SameName`; the validation probe is built once a
  statement, not once a variant; `AdminPlan::id`, written and never read,
  is gone, and an administrator's `PlanRow::order` is no longer kept
  beside its variants.
- **Not taken**: `IN`, `BETWEEN` and `NOT` in a rule stay `InvalidArgument`
  - they are reserved words the rule grammar does not name, and naming
  each one to say `NotImplemented` is a list with no end. A refused commit
  rebuilding the outcome without the plan fields is left: a `SELECT`
  commits nothing. `Snapshot`'s merge picking the latest part's source
  rather than the latest non-empty one is left: the two differ only for a
  shape one core has never compiled. A `base/` home for the tree's ten
  private `IEquals` copies is outside the stage.
- **Left as gaps** (`known-gaps.md`, Captured plans): what no cell reaches
  - plan DDL beside a held `DROP TABLE` `X` and from two cores among it.

### CX-S2 - done 2026-11-18: the inventory; no code

On worktree `cx-plan-change-option`. S2 builds nothing: CX-Q9 (a) keeps
class (b) out of CX until a scenario statement is named as its consumer,
and none is.

- **The inventory** is `parser-v2.md` I12's "The rewrite inventory"
  table, read off `CompileBound`, `PropagateEqualities`,
  `PredicateReadyAt`, the scan kernel and the executor's hoist, build and
  probe memo at `b11a5500`: eleven rows, each with what decides it and
  what it changes.
- **Why nothing in it moves a plan on its own**: every item is in
  `CompileBound`, which every path of `CompileBlock` calls - the early
  return with no options, an administrator's plan, a captured order, the
  memo and the search - and which is handed no `PlanOptions`, so none can
  read a statistic or `join_order`. One cell pins it
  (`AdminPlanTest.ForOneOrderTheChainIsTheSameWhoeverChoseTheOrder`):
  `ANALYZE`'s plan less its `planned` line is one text for the written
  order under `written` and under a plan saying the same at each value,
  and one text for the other order whether the search, the capture or a
  plan chose it; both carry the same two derived bounds. The same again
  with `ORDER BY ... LIMIT`, where one order elides the sort and the other
  keeps a top-N heap.

**Where the inventory differs from the order's text.**

- **§1.4 and CX-S2 class the false uncorrelated `EXISTS` short-circuit
  (a), "pure in the text and append-only". It is neither.** It adds no
  conjunct: it is the hoist's consequence - the hoisted sub-chain runs
  before the outer chain, and a false one ends the statement without
  opening the outer relation - so it reschedules a read and is decided by
  the run's rows. It is sanctioned all the same (I12: an uncorrelated
  subquery's answer is one value), reads no statistic, and only ever runs
  in written order, since a block with a subquery predicate is not
  reordered. The table classes it "scheduling, sanctioned" beside the
  hoist, and (b) is kept for what S2 forbids: a reschedule the engine
  chooses.
- **Two classes the order did not name** carry most of the rows:
  *placement* (where a conjunct is evaluated, the scan kernel, the decode
  masks, sort elision - a function of the text and the order that changes
  no read) and *locate* (the key set, the probe memo, the inner build).
  Splitting only into (a) and (b) would have put the inner build and the
  key set in (b), which S2 says is never automatic, and both are
  automatic by I12 and their own specs.
- **Found and not listed in the order**: an uncorrelated `EXISTS` under
  `OR` is not hoisted and is still answered once a statement where it
  stands; a repeated probe key skips its descent (the probe memo).
- **Not inventoried**: the query layer (`parser-v2.md` I19), which is
  another executor over native single-relation walks and compiles no join
  block; the write paths.

**The review of S2** (`critics-developer`, of `d08a31a2`; its corrections
and the ones taken from it are in the commit after it). It confirmed the
inventory's conclusion - nothing that rewrites a statement reads the plan
options, a statistic or `join_order` - and both corrections above, and
found four statements of the table false.

- **Fixed by the review - sort elision was classed "the sink; no read".**
  An unsorted walk under `LIMIT` stops at `offset + limit` emitted rows; a
  sorted one reads every row, a top-N heap included. So eliding the sort
  bounds the read, and the row says so.
- **Fixed by the review - "all of it is in `CompileBound`" and "none
  reads ... time".** The literal-on-the-left mirror is the parser's;
  `NOW()` reads the clock once into the statement's context; and the probe
  memo, the build's decline, the hoist's short-circuit and the scan kernel
  are decided at run time, the first two by the run's rows. The sentence
  now separates the compile-time items, which are `CompileBound`'s, from
  the run-time ones.
- **Fixed by the review - Waystone replay was missing**: armed after the
  compile for an opted shape and asked after the probe memo, before the
  descent. A row of its own, "locate, advisory".
- **Fixed by the review**: the probe memo is one entry a step, matching
  the key its previous descent found, not "a repeated key"; the scan
  kernel is also under the `scan_kernel` boot flag; a comment of the cell
  said `reordered=1` is erased, which it is not and should not be.
- **Taken**: the table's scope is said - a native `SELECT`'s chain, the
  query layer outside, and the key set and the inner build listed though
  they are ladder arms; the cell runs a second statement with `ORDER BY
  ... LIMIT`, so the elision and the top-N heap are in it.
- **Not taken**: comparing the decode masks through `exec::Compile` - they
  are not printed by `ANALYZE` and the cell stays on what a client sees. A
  statement with an uncorrelated `EXISTS` cannot be put through the cell's
  loop: a block with a subquery predicate takes no plan and is not
  reordered, which is the reason the hoist cannot differ by `join_order`.
- **Not checked by the review**: the scan kernel's behaviour beyond its
  comments, `chain_frame.cpp`, the Cabin run-time fallback.

### CX-S3 split; CX-S3a - built 2026-11-18

S3 is taken in two parts. **S3a** is the pin: `ACCESS` in a variant,
legality at every compile, `ANALYZE`'s mark, `DROP INDEX` RESTRICTed.
**S3b** is CX-Q6 (a) - a controller-made Cabin eligible to the ladder
only through a plan that names it - which changes what the ladder takes
with no plan in force and belongs to `cabin-controller.md` as much as
here; it is the section after this one.

**S3a, as built** on worktree `cx-plan-change-option`:

- **The clause**: `ACCESS <binding> <pin> {, <binding> <pin>}` after a
  variant's `ORDER`, in `CREATE PLAN` and in `REPLACE VARIANT`; a pin is
  `TUPLE_KEY`, `INDEX [<name>]`, `CABIN`, `SCAN`, or `NOT` before one of
  the first three. One pin a relation.
- **A pin restricts the ladder** (`exec::ArmPin`, `CompileBound` section
  4): it travels with its relation through the permutation
  (`BoundRelation::pin`), each rank of the ladder is tried only where the
  pin admits its class, and a pin naming a class is checked against the
  arm the ladder then took. No rank's own rule is changed, so no arm is
  taken that the ladder could not take (CX-R2).
- **A pin no arm meets** refuses that compile of the variant: at `CREATE
  PLAN` the statement's refusal, naming the relation and the pin; later,
  a decline - the written order, unpinned, counted.
- **The row**: a variant's line gains ` access=<w>:<pin>;...` only where
  it pins, so a row S1b wrote reads as it did. No superblock moves.
- **`DROP INDEX`** is RESTRICTed by an enabled plan that pins the index by
  name (CX-Q4 (a)); `ALTER PLAN ... REPLACE VARIANT` refuses a named index
  its relation does not have.
- **Seen**: `ANALYZE`'s `arm=plan` on a pinned step; the clause in
  `sys.plans`' `variants`.

**Where S3a differs from the order's text.**

- **CX-S3: "`ANALYZE` names, per step, whether its arm came from the
  ladder or from the plan."** Built as a mark on the pinned step alone,
  `arm=plan`; an unmarked step is the ladder's. Printing `arm=ladder` on
  every step of every statement would move the text the four contract
  suites compare for a fact that is true of every step they have.
- **And a pinned step's arm is still the ladder's choice**, among what the
  pin left: `arm=plan` says the plan constrained it, not that the plan
  chose it. The order's "pin an arm" reads as a force; CX-R2 says it is
  not one, and the build follows CX-R2.
- **CX-Q4 (a) names "a Cabin drop" beside `DROP INDEX`. Not built for a
  Cabin**: `CABIN` names a class and no Cabin, so there is nothing for a
  drop to be restricted by. CX-Q4 (b) covers it - the variant is declined
  and counted. A pin naming a Cabin goes with S3b, which needs it.
- **`INDEX` may be written without a name** (any index), and `NOT INDEX`
  takes none; the order's sketch showed the named form only.
- **A plan is still a join block's.** S1b's refusal of a one-relation
  statement said access pins were S3's; S3a does not lift it, because the
  compile asks for a plan only where a block has two relations and a
  probe on every point read is what CX-R6 forbids
  (`known-gaps.md`).

**The review of S3a** (`critics-developer`, of `4880575c`; its fixes and
the ones taken from its findings are in the commit after it). It found no
way for a pin to change a result: masking a rank never touches a step's
residual, so a key, a value list and a derived conjunct are still asked of
every located row; the sort decision reads the arm the step ended with;
the pin's pointer does not outlive the compile; and with no pin the chain
is what it was.

- **Fixed by the review - a `TUPLE_KEY` pin was refused where a tuple key
  arm was legal.** The filter scan and the build annotation were not
  behind the pin, so under `TUPLE_KEY` they took a step with an equality
  filter before it reached the strict tuple key range the ladder ranks
  last, and the met-check then refused: `CREATE PLAN` failed for a
  statement whose step is a `Range` under the pin. The walk's two ranks
  now stand aside under a pin naming a keyed class. A cell.
- **Fixed by the review**: `ALTER PLAN ... REPLACE VARIANT`'s refusal of a
  missing index named the variant's byte, not the index name's.
- **Taken**: `CREATE PLAN`'s probe checks the note is there before it
  reads it; a variant's refusal carries one byte, the compile's where it
  names one; `DROP INDEX`'s RESTRICT names the index's byte; `NOT INDEX
  <name>` is refused at the name, not by the next clause's message; a
  correlated Cabin probe the pin rules out no longer stands in a value
  list's way at the index (`LiteralIndexChoiceOf`), so an `INDEX` pin is
  not declined for a Cabin it forbade.
- **Taken, as cuts**: the four spellings of an arm - the DDL's reader, the
  row's writer and reader, the printer - are one table (`ArmName`,
  `ArmFromName`), and the row stores the DDL's word; the two "which
  enabled plan" walks are one (`EnabledWhere`); the met-check asks
  `ArmClassOf`, a switch with no `default`.
- **Confirmed and stated, not changed**: under `NOT TUPLE_KEY` or `CABIN`
  the strict range is ruled out, so a value list on a cabined column is
  served as a Cabin set where CU-Q12 kept the unpinned ladder on the
  range. The rows are the same and the arm is the one asked for; I12 says
  so.
- **Not taken**: an `Admits` over a null pin, to drop the one `static
  const` no-pin object - it saves a line and moves a null check into
  every rank.
- **Not checked by the review**: the Waystone trail and the statistics
  fold under a pinned chain; a met `CABIN` pin, which no cell builds; a
  release build.

### CX-S3b - built 2026-11-18

CX-Q6 (a): a controller-made Cabin is eligible to the ladder only through
a managed plan that names it. Built on worktree `cx-plan-change-option`
on the operator's "continue the CX handoff" (2026-11-18, given in Korean),
under the standing go-ahead (`raft-marks-2026-11-18.md` §16).

**What the item costs, found while building it and not stated when it
was adopted.** The cabin controller's candidates are one-relation
filters - `SELECT ... FROM t WHERE c = <value>` is the shape its every
end-to-end cell runs - and S1b and S3a gave a plan to a join block only.
Built to its letter on that base, (a) would have left the controller
with nothing that could ever read its Cabins. So S3b carries two things
the item's one line does not name: **a plan for a statement over one
relation**, and **a pin that names a Cabin**. And one consequence stands
whatever is built: **with the controller on and no plan naming its
Cabins, the controller accelerates nothing** (`known-gaps.md`), where
before this stage switching it on moved a hot filter scan to a Cabin
probe by itself. That is what (a) asks for - the item's own reason is
that the switch was an implicit plan change - and it is the operator's
to confirm or reverse, since it turns the controller from something that
acts into something that prepares.

**S3b, as built:**

- **The gate** (`step_compiler.cpp` `CabinGate`): every Cabin rank of the
  ladder - the literal probe, the expression and predicate probes, the
  correlated probe, the cover, the value set - asks it which Cabins the
  step may take. A Cabin of `auto` origin is admitted only where the
  step's pin names its column; with no pin, under `CABIN` unnamed and
  under `NOT` another class, the relation reads as though the Cabin were
  not there. Under every `join_order`, and in a subquery of a write as
  in a read (a write's own `WHERE` takes no Cabin arm). A declared Cabin
  is admitted as before.
- **The named pin**: `ACCESS <binding> CABIN (<column>)`. It admits the
  column Cabin on that column, whoever made it, and nothing else, so the
  class's met-check is the named Cabin's. `NOT CABIN (<column>)` is
  refused; an expression or predicate Cabin cannot be named
  (`NotImplemented`); `REPLACE VARIANT` checks the column carries a Cabin
  (`NotFound`). The row: `<w>:CABIN:<column>` in a variant's `access=`.
- **A one-relation statement's plan**: `CREATE PLAN <name> FOR <select
  over one relation> VARIANT ... ORDER (<the relation>) [ACCESS ...]`.
  `AS SEARCHED` is refused `Unsupported` (no order to search) and a
  subquery predicate `NotImplemented`. The compile asks
  `AdminPlans::any_single()` - one relaxed load - and probes only while a
  one-relation plan is enabled; it compiles the statement unpinned, then
  under the variant's pin, and declines the pin where no arm meets it or
  it adds a sort the `LIMIT` does not bound (CX-R2, CC-Q10).
- **The drop** (CX-Q4 (a)): an enabled plan that names a Cabin RESTRICTs
  `DROP CABIN ON t(<column>)`, and the controller's DROP of it is skipped
  and counted (`sys.meta` `cabin_optimizer_drops_restricted`); a budget
  swap whose victim is so restricted builds nothing.
- **Seen**: `ANALYZE`'s `cabin_optimizer=true` now only beside `arm=plan`;
  the clause in `sys.plans`' `variants`.

**Where S3b differs from the order's text, each CLA's choice and none
marked by the operator.**

- **A one-relation statement takes a plan**, which S1b refused and S3a
  kept refused for CX-R6. It is what makes (a) usable at all. CX-R6 as it
  now stands: with no one-relation plan enabled a point read pays one
  relaxed load and no probe; with one enabled, every top-level
  one-relation `SELECT` with a fetch id pays the probe (`known-gaps.md`).
- **"A plan that names it" is read as a pin naming the Cabin's column**,
  and `CABIN` unnamed does not admit a controller-made Cabin. The
  alternative - any `CABIN` pin admits it - would let a plan written for
  a declared Cabin start reading a controller's the day one appears on
  another column of the step.
- **(a) holds under `planned` too.** `planned` is the operator's opt-in
  to statistics choosing the join order (W3); the item names no value,
  and a Cabin's appearance is not a statistic.
- **The controller's DROP is RESTRICTed, not only `DROP CABIN`.** CX-Q4
  (a) names "a Cabin drop"; the controller dropping a Cabin a plan reads
  through is the implicit change (a) exists to exclude. The decision
  core is untouched: the executor skips the action.
- **The foreign key's reverse check still reads a controller-made Cabin**
  (`foreign-keys.md` §3a): it is enforcement, not a statement's plan.
- **Not built**: a named expression or predicate Cabin; a RESTRICT on
  `RENAME COLUMN` of a named column (the pin names nothing, and the
  plan's statement, which names the old column, matches no statement); a
  relation lock under a column Cabin's drop, so the
  RESTRICT has a window (`known-gaps.md`).

**The review of S3b** (`critics-developer`, of `ae77630f`; its fixes and
the cuts taken from it are in the commit after it). It found no way for
the stage to change a result. It confirmed by reading that the gate is
the one place a Cabin arm is chosen, that the one-relation compile's step
ids and side effects are those of a single compile, and that the enabled
counters cannot drift; and it ran the full suite on its fixes, 0 failed
of 4683.

- **Fixed by the review**: an empty or unclosed `CABIN (...)` was refused
  `NotImplemented` (as though it named an expression); it is
  `InvalidArgument`, at the offending token. A cell.
- **Fixed by the review, in the documents**: an unnamed controller-made
  Cabin can still be asked for an EXTEND, by the foreign key reverse
  check's misses, which finds nothing to seed; a restricted
  quality-collapse DROP leaves its entry ACTIVE, not DECAYING; a write's
  own `WHERE` takes no Cabin arm, so the gate binds a write's subqueries;
  after a `RENAME COLUMN` a plan naming the column matches no statement,
  rather than being declined statement by statement.
- **Taken, as cuts**: the one-relation branch is section 1a' itself - a
  one-relation block enters the join path's plan lookup while
  `any_single()` holds and leaves right after it, with no statistics
  stamp - instead of 45 lines repeating it; `ArmPin`'s two name fields
  are one (`name`), which also removes the warnings the commit added;
  `CabinGate::For`; `PlannableBlock` as an if/else; the ladder's `rel`
  and `slot` locals used at every call; `Apply`'s two flags as one
  `std::exchange`.
- **Not taken**: rate-limiting a restricted DROP, which the decision core
  emits again at every tick while its condition holds. It is noise in a
  counter and the decision log, not a wrong state, and stopping it needs
  the pure decision core to be told why an action did not land; stated in
  `known-gaps.md`.
- **Not checked by the review**: a streamed one-relation plan; one across
  a restart; plan DDL racing a Cabin's drop; `CREATE PLAN` over a `sys.*`
  view; overhead.

### CX-S5 - built 2026-11-18

Built on worktree `cx-plan-change-option` on the operator's "go on with CX-S5" (2026-11-18, given in Korean), under the standing go-ahead (`raft-marks-2026-11-18.md` §16).

**S5, as built:**

- **The clause**: `BUILD <binding> ON | OFF {, <binding> ON | OFF}` after a variant's `ORDER` and any `ACCESS`, in `CREATE PLAN` and `REPLACE VARIANT`; one a relation (`ArmPin::Build`, beside the access pin).
- **`OFF`** takes the build's rank from the step's ladder: a walked join walks per outer row, or takes the strict tuple key range below it.
- **`ON`** holds the step to the build - every keyed class stands aside, as under `ACCESS ... SCAN` - and is met only where the step then carries the build annotation. `BuildKeyOf`'s rule and the filter scan's rank above it are unchanged, so no build is made that the ladder's own last rank could not make. Unmet, it is S3a's unmet pin: refused at `CREATE PLAN`, declined at a compile. Beside an `ACCESS` pin naming a keyed class it is refused (`InvalidArgument`).
- **The cap** (CX-Q8 (a)): a met `ON` builds subject to `join_build_max_rows` at run time, as every build does - `0` or an outgrown map walks, `ANALYZE` showing `inner_built=0`. The pin is not a plan change when it does.
- **The row**: ` build=<w>:ON|OFF;...` after `access=`, only where a variant pins a build, so every row written before reads as it did. No superblock moves.

**Where S5 differs from the order's text, each CLA's choice and none marked by the operator.**

- **`ON` is read as a restriction to the build, not a bare check.** The order gives the clause and its example (`BUILD u OFF`) and no meaning for `ON`. A check alone would add nothing - the ladder already builds where it can - and the order's subject is choosing the join method, so `ON` takes the nested-loop probes (tuple key, index, Cabin) away for that step, within CX-R2: the build is an arm the ladder's last rank takes for the shape.
- **The filter scan stays above the build under `ON`.** `join-inner-build.md` §8 declines building for a step a literal equality already filter-scans, "a decision, not an impossibility"; S5 does not reopen it, so such a step cannot meet `ON`.
- **A captured order holds no build pin** (CX-Q12 (a) as marked): its build side is the compile's given its order.
- **CX-S5's "A plan that orders a relation inner picks its build side through ORDER"** needed no code: an order already decides which step is inner.

**Also in this stage**: §45's sentence that the merge at `4e0a1649` was not suite-run is corrected - that run finished 0 failed of 4706 after the push.

**The review of S5** (`critics-developer`, of `363ab7d2`; what it fixed and the cuts taken from it are in the commit after it). It found no code defect and no way for a build pin to change a result: `ON` only takes away the ranks above the build, which is still made by the ladder's own last rank under `BuildKeyOf`'s rule; `OFF` leaves the next legal rank; every caller of `ArmPin::pins()` stays right with a build-only pin; `DecodeBuild` and `ResolvePlanVariant` refuse the same `ON`.

- **Fixed by the review, in the manual**: when `ON` can be met was overstated - it left out the same-type rule, the boolean and function conjuncts that rule a build out, and that the tying condition is to the relations before the inner one; and a declined `ON` build walks the inner relation per outer row rather than looking rows up, the keyed arms having been set aside.
- **Taken, as cuts**: one `BindingPosition` for the three places a binding was looked up; `ArmPin::keyed()`, so the DDL and the row reader cannot disagree on what `ON` may sit beside; `BuildName` for the four spellings of `ON`/`OFF`; the `ACCESS`-twice check by `arm`, not by `pins()`, so it no longer depends on running before the `BUILD` loop.
- **Taken, as comments**: `BuildKeyOf`'s tuple key decline was called defensive; since S3a and S5 a pin can bring a tuple key equality to it, and that decline is what keeps a tuple key join from meeting `ON`. `Step::arm_pinned` now says a build pin sets it. I12 names `REPLACE VARIANT` beside `CREATE PLAN` for the keyed-pin refusal, says a build-only pin is marked `arm=plan`, and drops a write's sub-chain, which no plan reaches.
- **Not checked by the review**: build pins across a restart end to end; `CREATE PLAN` with `BUILD` racing a DDL; a variant declined at a later compile because of `ON`; three or more relations with `ON`; overhead.

### CX-S6 - done 2026-11-18: measured, no cache built

On the operator's standing go-ahead of 2026-11-18 (go-ahead-achieving-milestone; `raft-marks-2026-11-18.md` §47). S6 is its first cell and the decision it makes (CX-Q10, settled as CX-Q37):

- **The cell** (`ck-tester`, release at `9712f5f0`, `bench/v3.4.12/results-cx-s6-compile-share-v3.4.12-112-g9712f5f0.md`): six repeated statement shapes - a point read, a captured two- and three-relation join, a two-variant plan, a one-relation plan and its unplanned twin - at `cores = 1` and `2`. `sys.statement_stats`' `compile_us` came to 7.6 % to 11.1 % of the client's p50 in every cell, over the 5 % line.
- **The premise re-measured before anything was built** (§7 of the same file; `bench/compile_split_bench.cpp`): `compile_us` holds the lex, the parse and the fingerprint, which a cache keyed by the fetch id still pays to find its key. `exec::Compile` alone - the most a cache could remove - is 0.57 µs to 5.3 µs in a hot loop, 1.4 % to 4.0 % of the client's p50 at `cores = 1` - superseded by the next bullet; the shapes are like the cell's, not the cell's, and the 4.0 % row is a captured reorder no cell ran (§7).
- **The review of S6 found the hot loop understated the compile**, so the part a cache could remove was timed inside the server (§8 of the same file): a spike - two counters around `CommandDispatcher`'s one `exec::Compile` call, never committed, its binary hashed - over the cell's own driver, data and plans. That ceiling is **2.8 % to 4.1 % of the client's p50 in every plain shape at both core counts, and over the line only where a plan governs the statement**: the two-variant plan at 5.7 % (`cores = 1`) and 6.3 % (`cores = 2`), the one-relation plan at 5.3 % at `cores = 2` alone. Against the server's own time every cell crosses (5.3 % to 15.2 %).
- **So S6 builds no compiled-chain cache** (CX-Q37 (a), revised to these numbers): "only if" is a necessary condition; what crosses is a plan's own extra work, part of it (the variant rule's lex) a variant-keyed cache would still pay, by about a point before the cache's own costs; and a chain bound again per statement is this order's one structure that could answer wrongly. (b), a cache for planned statements, and (c), the line against server time, are the operator's. `PlanMemo` keeps its one consumer, `planned` (CX-Q3); under `fixed` the captured map is what avoids the search, and variant selection stays per execution (S6's other bullets), all as built since S0 and S1b.
- **Found on the way**: `sys.statement_stats.compile_us` is a sum over calls, which `sys-views.md` §6.1 did not say; the clause is added there (the code and its test were right, so nothing else moves). The costliest rows compile twice (an order other than the written one compiles the written order first), recorded in `known-gaps.md`.
- **Where this rests on a choice**: the line is CX-Q10's, against the client's p50, and the client was Python (33 to 36 µs of each p50); a faster client moves every share up. The in-process microbenchmark (§7) is kept as the tool that priced the front end (the lex, parse and fingerprint, 54 % to 68 % of `compile_us` in-server) and is superseded for the removable part.

### CX-S7 - built 2026-11-18

On the operator's standing go-ahead of 2026-11-18 (go-ahead-achieving-milestone; `raft-marks-2026-11-18.md` §47), CX-Q11 (a) (on demand only) and CX-Q29..Q33 (a) as adopted there.

**S7, as built:**

- **`ADVISE PLAN FOR <select>`** (`CommandDispatcher::HandleAdvisePlan`; `exec::ParseAdvisePlan`): admin's, by example with literals (the `?` refusal and the blanked command text shared with `CREATE PLAN`, `PlanExampleText`). The compile is handed the statistics and no administrator's plans, captured map or memo, so it searches and records nothing; the plan in force (CX-Q32 (a)) is handed as `PlanOptions::advise_against` and its pins, compiled through the ladder in the search section and priced under the same `CostInputs` into `PlanNote::in_force_ratio` - as the written order where it would be declined, and as the written order past the captured table's ceiling under `fixed` (the review's fix). The reply carries the `CREATE PLAN` text, issuable where no plan holds the shape, and only where `CREATE PLAN` takes the block: a subquery predicate's gets `why=` and no text.
- **`sys.plan_advice`** (`exec::PlanAdvices`, one table for the instance on the surface beside the administrator's plans): the latest advice a shape, oldest first, bounded `[provisional]`; its proposal over the shape text.
- **The loop W0 describes is closed in the engine**: ASH and `sys.statement_stats` show a statement, `ADVISE PLAN` proposes, a DBA or an agent issues the `CREATE PLAN` it gave, and `ANALYZE` and `sys.statement_stats` show the result - the first S7 cell runs it end to end through `ANALYZE`, the issued text becoming the plan in force.

**Where S7 differs from the order's text, each CLA's choice under the go-ahead:**

- **The proposal is an order alone**, a variant with no rule and no pin: the search decides the order (and with it which step is inner, CX-Q12 (a)), not an arm or a build. A plan naming a controller-made Cabin (CX-Q36) is not proposed.
- **"the statistics generation it read"** is the published snapshot's generation (`PlanNote::generation`), 0 where nothing is published; the order's "inputs generation" column is `generation`.
- **The ratio is text** in the view: no `sys` view carries a decimal type.

**The review of S7** (`critics-developer`, of `435336c9`; its fix and the cuts taken from it are in the commit after it). It found that `ADVISE PLAN` changes no state a later statement reads - no capture, memo, plan counter, statement shape, steward input or log - and that the in-force pricing uses the search's own inputs and the real decline rules; it ran the full suite on its fix, 0 failed of 4712.

- **Fixed by the review - a full captured table read as `uncaptured`**: past the ceiling the next compile runs the written order (CX-Q21), and the advice priced against the search's answer, a ratio of 1 for an order that does not run. A cell.
- **Decided after the review (B2), on the go-ahead**: a subquery predicate's join was given a `plan=` text that `CREATE PLAN` refuses, and no reply said why a block was not searched, which CX-Q33 (a) promises. Now every unsearched block carries `why=`, and the plan text - and the view's proposal - only where `CREATE PLAN` takes the block. CX-Q33's row is corrected the same way, and to drop a joined `SELECT *`, which the parser refuses before the advice. Cells.
- **Fixed by the review, in the documents**: the plan text is issuable only where no plan holds the shape; a ratio of 1 for an unsearched block prices nothing; one plan a shape, so `DROP PLAN` first (the manual).
- **Taken, as cuts**: `PlanAdvice::bindings` (never read) removed; one `written_position` for the memo, the capture and the advice, and the written order priced without a recompile; one `LexAll` for both plan parsers; one `RatioText`; `sys.plans`' forwarding label lambda inlined; `ADVISE PLAN FOR takes a SELECT` worded as the parser words it, and the unknown `ADVISE` target refused `InvalidArgument` at its byte; the proposal named `advised_<fetch id>` in decimal, as a capture's is.
- **Not checked by the review**: eviction at the bound, a `readonly` session's refusal, the KWP path, `ADVISE` racing a `DROP TABLE`.

### CX's close - measured 2026-11-18; a point-statement regression stops the landing

`ck-tester`, Release, `cores = 1`, A = `8b584cf6` (the commit CX opened from) against B = `a32528a4` under `fixed` and B' under `planned`, A2 the noise control, two attribution arms X = `58d258a3` (S0-S3a) and Y = `ee89842e` (X with DF-S1); `bench/v3.4.12/results-cx-close-v3.4.12-115-ga32528a4.md`.

- **Join blocks**: no regression beyond the floor (-0.1 % to -0.8 %); every searched shape ran `order=captured` under `fixed`, every result multiset equal on every arm.
- **§6's gate on `planned`**: B' equals B within 0.3 % on every cell, so the value `planned` kept its behaviour - and what the point statements lost is not the captured table.
- **Point statements are slower**: a point read and a two- and three-relation tuple key join lose 1.2 % to 2.0 % of throughput against A in two runs, above an A2 floor of 0.2 % to 0.9 %, at +0.4 to +0.8 µs of server CPU each. X is within the floor of A; Y (DF-S1, not CX) carries part; the rest falls in S3b-S7 and this run does not separate it. CLA read what S3b-S7 put on a point read - the one-relation plan gate (an atomic load), `CabinGate::For`'s early return and a pin's build check per rank, one routing compare for `ADVISE` - and names no cause of that size. **Not remedied within the milestone**: a bisect of `ee89842e..a32528a4` with a spike on `sys.statement_stats` would attribute it, and the milestone's measurement budget is spent; the landing waited for the operator's word on it, and **the operator's "push no verify" landed CX on `main` with the regression standing** (`raft-marks-2026-11-18.md` §49).
- **The plan-lookup probes (CX-R6)**: an enabled join plan costs a point read or a tuple key join nothing resolvable; an enabled one-relation plan costs a point read +0.17 to +0.25 µs of CPU against a control's +0.06 to +0.11 µs - small, the same sign in both runs (`known-gaps.md`).
- **Not measured**: `cores = 2` (the control moved 2.8 %, unresolved); the price of a capture from an empty snapshot, which CX-Q19 as built never makes (a shape defers for about a second instead, untimed); a point read under a plan that matches; a restart's `sys.plans` load.

### The close regression bisected, 2026-11-19: it enters at DF-S1, and no CX stage is shown to carry any

On the operator's word (`raft-marks-2026-11-19.md`), the bisect the close
did not run: ten arms in one server lifetime - at CX's opening, after
CX-S3a, after DF-S1, and at every stage from CX-S3b to `main` at `11f72ce9`,
with two same-binary controls - then four server lifetimes of a DF-S1 knob
spike (`bench/v3.4.13/results-cx-close-bisect-v3.4.13-1-g11f72ce9.md`,
worktree `cx-close-regression-bisect` at `11f72ce9`). The regression is
reproduced (+0.32, +0.39 and +0.53 µs of server CPU a point statement on
`main` against A, pooled over the four lifetimes) and, in the lifetime that
has the arms, enters at one step, `58d258a3` to `ee89842e`: DF-S1, merged
into CX's span from DF's work branch. CX-S0 to CX-S3a are at zero (X - A:
-0.13, +0.09, +0.11 µs pooled); CX-S3b to the close move by up to one and a
half times the widest same-binary control in both directions and sum to
nothing (B - Y: -0.01, -0.02, -0.21 µs, upper ends +0.21, +0.11, +0.10); CY
adds nothing (M - B within 0.04 µs). **The close's reading that the later CX
stages carried +0.29, +0.50 and -0.03 µs is withdrawn**: its Y and B were
one process each over 12 rounds. CX-R6 (zero for the unmanaged point path)
therefore holds for the state with no plan enabled at this method's
resolution, 0.2 to 0.3 µs. What DF-S1's cost is made of is in
`workorder-df-statistics-coverage.md` §7. `cores = 2` is still unresolved.
