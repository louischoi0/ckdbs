# Work order CX - the plan as a managed object: the contract restored, plans set by bindings, the engine an advisor

**Written 2026-11-18, not opened**, on `main` at `286889cc`
(`v3.4.12-12-g286889cc`), on the operator's words, recorded in English
(`raft-marks-2026-11-18.md` §1-§2):

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
  order search. W0 turns (6) into an advisor. §3 maps each one to a stage.

**Claim tags**: `[source-read]` with `path:line` at `286889cc`; `[design]`
for reasoning about code not yet written; `[measured]` with its commit;
`[quiet-wrong]` where a wrong choice turns a correct answer into a wrong
one with no error; `[operator]` for W0-W2.

**The letter.** `[source-read]` CW is the last work order under
`instructions/v3.2.0/` at `286889cc`, and no file, branch or commit subject
fetched names a `CX-` token or a `workorder-cx` file. CLA's choice.

**CX against other orders and specs.** CX **overrules, as default
behaviour, CB-S2's planned order and build side and CC-S3/S4's
statistics-priced order and order memo** (`workorder-cb-join-planner.md`,
`workorder-cc-plan-statistics.md`, both CLOSED). Those orders stay as
history. Their estimator stays and becomes the advisor in CX-S7. CX
**restores** `docs/spec/parser-v2.md` I12's first sentence as it read
before CB, and puts a managed plan beside it as the only other source of a
plan. It leaves standing: equality propagation and CB-S1's range bounds
(§3, CX-S2), the decorrelation prohibition as an *engine* rule, the
statement-local inner build's mechanics, invariants 8 and 9, and every
trust-class rule. It is not part of AR0 §8's chain.

**A correction CX owes the conversation that produced it.** Before W0, CLA
told the operator that the contract held today and that no cost-based
optimizer existed. That was false. It was said without reading the tree,
and §1.1 is what the tree says. W0 was given as a decision to *keep* the
contract. On the tree, the decision is to *restore* it. CX-Q0 puts that
difference to the operator before anything is built.

---

## 0. What CX is

W0 has three parts, and CX applies all three to every plan decision the
engine makes:

1. **The contract** `[operator]`. With no managed plan, a statement's plan
   is a pure function of its text and of the catalog's DDL state. Nothing
   else is an input: no statistics, no memo, no background structure, no
   load and no time.
2. **The managed plan** `[operator]`. A DBA or an agent may attach a plan
   to a statement shape. The plan may have variants chosen by the
   statement's bound values, under a rule the plan itself states. The
   plan is a catalog object, like an index or an assertion: created,
   altered, disabled, dropped, exported and restored by explicit acts.
3. **No implicit change** `[operator]`. The engine never moves a statement
   from one plan to another on its own. What it may do is **advise**: it
   computes a better plan and shows it, and a person or an agent decides
   whether to adopt it (CX-S7).

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
  I12 forbids it, and W0 forbids it again. As a managed-plan option it is
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
(`docs/spec/plan-stats.md` PS5). That is the implicit change W0 forbids.

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
  range span band enters the cost only on the planned path. So without
  `planned`, the plan is `f(text, catalog)` with the literals inside "text".

### 1.5 Inputs to a plan that are neither text nor DDL

These are the cases W0's third clause reaches besides §1.1. Each is an
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

- **CX-R1 - no implicit plan change** `[operator]`. A statement's plan is
  the managed plan's chosen variant if an enabled plan matches it.
  Otherwise it is the default, `f(text, catalog DDL state)`. No other
  input reaches a plan. Statistics, the memo, advisory structures, load,
  time and core placement are all excluded.
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
  an agent's session.
- **CX-R6 - zero cost for the unmanaged point path** `[design]`. A point
  read or a tuple key chain leaves the compile before any plan lookup, as
  it leaves before the statistics today (`plan-stats.md` PS9). The lookup
  is one probe of a per-core read-only map, and only for a statement whose
  fetch id has a plan.

## 3. Stages

S0 and S1 are the foundation W0 needs. S2-S7 are W2's priorities 1-6, in
that order. A stage opens on the operator's word, and each one closes on a
measurement where §6 names one.

### CX-S0 - the contract restored (prerequisite)

- `join_order` defaults to `written`. The `planned` arm no longer chooses
  a plan. CX-Q1 decides whether it is deleted or kept as a diagnostic only.
- I12 is rewritten around the two plan sources of CX-R1. CB's and CC's
  amendments move to a history note. `plan-stats.md` PS9 narrows to: "the
  advisor and `ANALYZE`'s estimate read a fact; no plan does".
- The search, `EstimateChain` and `PlanStats` stay, with no plan consumer
  until S7. Under "no structure without a confirmed consumer" this is
  stated rather than left implicit, and CX-Q2 decides whether the steward
  keeps running in the meantime.
- **Expected cost** `[measured]` at `dd3af538`, from CB §1: scenario 3's
  `join-no-literal` written in the bad order runs 4,845 µs, against
  1,090 µs planned. S0 gives that statement back its written-order time.
  A managed plan (S1) is how it gets the fast order again. The regression
  is the price of W0, and §6 measures it.

### CX-S1 - the plan as a catalog object, with bind-value variants (foundation)

- **`sys.plans`** `[design]`: one row per plan, stored like
  `sys.assertions` (logged, `source_text` form). It holds the name, the
  statement text with `?` in its value positions, the fetch id, the relation oids in
  written order, the variant list, the state (`ENABLED`, `DISABLED`,
  `INVALID`), its creator, and a reference list (indexes and Cabins it
  names).
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
- **`ANALYZE`** prints `plan=<name>/<variant>` or `plan=default`.
  `sys.statement_stats` gains `plan_name` and `variant`.
- **Agents**: `sys.plans` is an ordinary table (BO-R3), so an agent reads
  it, the advice (S7) and ASH (CJ) through SQL, and acts through the same
  DDL a DBA uses. Nothing agent-specific is added.

### CX-S2 - priority 1: rewrite rules, split by whether they are text-pure

- **Inventory** `[design]`: every rewrite the compile performs, classed
  (a) pure in the text and append-only, or (b) changing read scheduling.
  At `286889cc`, equality propagation, CB-S1's range bounds and the false
  uncorrelated `EXISTS` short-circuit are class (a) (§1.4).
- **Class (a) stays automatic**: it is part of the default plan, which is
  `f(text)`, so CX-R1 holds.
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

- **Explicit collection** `[design]`: `GATHER STATISTICS ON t [ (c, ...) ]`
  collects row count, per-column NDV, MCV list and an equi-depth
  histogram, run as a background task that yields to OLTP (CA's W1 rule).
- **The model is an object**: stored per CX-Q7, with `LOCK STATISTICS`,
  `EXPORT` and `IMPORT`, so a plan decision can be reproduced on another
  instance or in a test.
- **Readers**: the advisor (S7) and `ANALYZE`'s estimates only. Never a
  plan (CX-R1). PS1/PS2's observed facts feed the same advisor beside it.

### CX-S5 - priority 4: join method selection

- The methods are the ones the tree has: the nested loop with a per-row
  probe (tuple key, index, Cabin), the walk, and the statement-local inner
  build.
- With S0 the build sits on the written order's inner step again, as
  before CB-S2.
- A managed plan sets `BUILD <rel> ON|OFF` per step. A plan that orders a
  relation inner picks its build side through `ORDER`, so CB's "smaller
  side" stays available as a plan rather than as an engine choice.
- §1.5's run-time decline: CX-Q8.
- A hoisted hash join is not built in CX (§0.1, CX-Q9).

### CX-S6 - priority 5: the plan cache

- With S0 an unmanaged compile searches nothing, so `PlanMemo` has no
  consumer and is deleted (CX-Q3).
- A compiled-chain cache keyed `(fetch_id, oids, plan, variant,
  cache_built_at, catalog_version)` is built **only if** S6's first cell
  measures compile time as a material share of a repeated statement's
  latency at `cores = 1` and `cores = 2`. Its threshold is CX-Q10, set
  provisional until measured.
- Variant selection is evaluated per execution from the bindings. It is
  never cached across bindings.

### CX-S7 - priority 6: the join order search, as an advisor

- `ADVISE PLAN FOR <statement>` runs CB/CC's search with S4's model and
  PlanStats' facts. It returns the proposed plan as `CREATE PLAN` text,
  with its estimated cost ratio and the statistics generation it read.
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

Every item is CLA's proposal and unmarked. A finding that would change one
goes back to the operator before it is built.

| item | question | options | CLA proposes |
|---|---|---|---|
| CX-Q0 | W0 said *keep*; the tree needs *restore* (§1.1). Confirm that CB-S2's and CC-S3/S4's planned order stops being the default | (a) restore, as CX states; (b) keep `planned` as the default and add managed plans over it | (a). (b) is the implicit change W0 forbids |
| CX-Q1 | the `planned` setting after S0 | (a) delete the arm; (b) keep it as `ANALYZE`-only diagnostics | (b) folded into S7: the search lives on as `ADVISE`, and the setting is deleted |
| CX-Q2 | the PlanStats steward between S0 and S7 | (a) keep it running; (b) stop it until S7 opens | (a): its cost is measured (CC §10), and S7 needs its history |
| CX-Q3 | `PlanMemo` | (a) delete at S6; (b) delete at S0 | (b): its only consumer goes at S0 |
| CX-Q4 | an enabled plan whose arm becomes unservable | (a) refuse the DDL that would break it, RESTRICT, as `DROP TABLE` does for an assertion; (b) mark it `INVALID` and run the default plan, counted and visible; (c) refuse the statement | (a) for `DROP INDEX` and a Cabin drop, (b) for anything (a) cannot see. (c) turns a plan problem into an outage |
| CX-Q5 | the `WHEN` language | (a) `ARG(k) op constant`, `AND`, `IS NULL` only; (b) arithmetic over arguments too (`ARG(2) - ARG(1)`) | (b) limited to `+ -` over integers, checked overflow, because range width is the case that needs variants |
| CX-Q6 | controller-created Cabins (§1.5) | (a) a controller Cabin is ladder-eligible only through a managed plan that names it; (b) leave the ladder as it is | (a). Under (b), switching the controller on is an implicit plan change |
| CX-Q7 | where S4's model lives | (a) a logged `sys` relation; (b) memory, like PlanStats | (a): a managed object that a restart erases is not managed |
| CX-Q8 | the run-time build decline (§1.5) | (a) keep it as a resource limit, like `sort_max_rows`, visible in `ANALYZE`; (b) count it as a plan change and refuse instead | (a): it changes the locate mechanism, never the plan's shape, and a plan can say `BUILD OFF` |
| CX-Q9 | class (b) rewrites and the hoisted hash join | (a) not in CX; (b) as plan options in S2/S5 | (a) until a scenario statement is named as the consumer |
| CX-Q10 | S6's threshold for building the cache | a share of p50 latency, `[provisional]` | 5 % at `cores = 1`, provisional until S6's first cell |
| CX-Q11 | background advising | (a) on demand only; (b) a background task too | (a) first |

## 5. `[quiet-wrong]` risks

- **A plan applied to the wrong statement**: closed by CX-R3. Matching
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

## 6. Measurement

AS-E epoch, `build-release`, results named by `git describe --tags`.

- **S0's close**: scenarios 1-4 at `join_order = planned` (A) against the
  S0 build (B). Every statement whose plan moves is listed with its p50 and
  p99. This is the price of W0, recorded and not optimised away.
- **S1's close**: the same statements with a managed plan that restores
  A's order, against A. The target is equal within noise. The
  plan-lookup overhead is measured on a point read (must be zero, CX-R6)
  and on a two-relation join.
- **S6's first cell**: the compile's share of a repeated statement's
  latency, which decides CX-Q10.

## 7. Contracts amended (when the stages land)

- `docs/spec/parser-v2.md` I12 - rewritten (S0), with managed plans (S1).
- `docs/spec/plan-stats.md` PS6 (deleted, S0), PS9, PS12.
- `docs/spec/join-inner-build.md` - the build side (S0, S5).
- `docs/spec/index.md` IX9 - "must not compile differently as the data
  changes" restored, which CC re-scoped.
- `docs/spec/cabin.md` §4e - under CX-Q6 (a).
- `docs/spec/sys-views.md` - `sys.plans`, `sys.plan_advice`, the new
  `sys.statement_stats` columns.
- `manual/` - `CREATE PLAN`, `GATHER STATISTICS`, `ADVISE PLAN`.

## 8. Stage log

Nothing has landed. CX is written and not opened.
