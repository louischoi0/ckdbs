# CN-1 — Concept Note: The Agent-Operable Optimization Surface

Status: CONCEPT, not a work order, not a CIP. Nothing here opens a stage.
Author: CLA, 2026-09-08, against `bc1040e`
Origin: operator's verbal concept of 2026-09-08 (two statements), recorded
and elaborated by CLA
Claim tags: `[operator]` for the operator's statements; `[source-read]`
with `path:line` at `bc1040e`; everything else `[design]`. Nothing is
`[measured]`.
Relation to AR0/AR1: this note builds nothing. Where it names existing
structures it names them to place the concept, not to change them (§5).
Proposed location if kept: `docs/concepts/` (does not exist; operator's
choice, §8) — **answered 2026-09-08: the operator chose `docs/blueprint/`,
which is where this file is and which this note's filing created.**

---

## 0. The concept in one sentence

`[operator]` An external AI agent, integrated **only as a plugin with a
user-like connection**, optimizes the database in real time by issuing
DDL/SQL/CMD — for example, given a relation and an explicit id set it is
known will be read or updated, it warms those tuples into memory and
creates read-assist structures (Cabins) ahead of the access.

`[design]` The product claim that follows is not "an AI-tuned database".
It is: **a database whose operator-facing surface is designed so that a
machine operator can drive it safely.** The agent is one client of that
surface and is replaceable. The surface is the product.

## 1. The engine boundary — what this concept is not

`[operator]` The engine stays deterministic and predictable. No AI
component is linked into, called from, or consulted by the engine. The
agent has no path into the engine that a human user does not have.

`[design]` Three consequences are fixed by that statement and are not
revisited below:

- **B1** Every agent action is a statement the parser accepts. There is
  no side channel, no RPC beyond the wire protocol, no hidden verb.
- **B2** The agent runs under a database role. Authorization is the
  existing role model; no new trust boundary is introduced.
- **B3** The engine's behavior given a sequence of statements is the same
  whether a human or an agent issued them. Nothing in the engine can
  detect, or condition on, the author being an agent.

## 2. Positioning against the industry

`[design]` The adjacent products fall into two groups, and the concept is
in neither.

| group | examples | automation lives | operator can see | operator can replace |
|---|---|---|---|---|
| internal ML | Oracle Autonomous DB (auto-indexing), Azure SQL Automatic Tuning, Aurora/AlloyDB ML features | inside the engine | opaque or summarized | no |
| external knob tuners | OtterTune and descendants | outside | config values only | yes, but the lever set is coarse |
| **this concept** | — | outside, as a plugin | every action, as DDL/CMD in the log | yes; any agent that speaks SQL |

The differentiator is not the agent's intelligence. It is the **grain and
safety of the levers** the database exposes: id-level residency, on-demand
read-assist structures, budgets, expiry, and a telemetry surface the agent
can query with the same SQL it uses to act. No relational engine today
designs that surface as a first-class deliverable; the internal-ML group
hides it, and the knob-tuner group does not have it.

Auditability is the strongest single point. With internal ML, "why did it
get slow at 03:00" has no answer the operator can read. Here the answer is
`SELECT ... FROM sys.statement_log WHERE role = 'agent' AND ts BETWEEN ...`.

## 3. Requirements the surface must meet

`[design]` Six requirements. R1–R5 were stated on 2026-09-08 in the
elaboration; **R6 is the operator's addition** and is elevated to a
governing requirement in §4 because it constrains the other five.

- **R1 Result-neutral.** No optimization statement changes the result of
  any other statement. A structure the agent creates is either
  transactionally maintained on the write path (a Cabin) or is advisory and
  never served alone (Waystone-class). There is no third class.
- **R2 Idempotent and reversible.** Issuing an optimization statement
  twice equals issuing it once. Every statement has an inverse (`DROP`,
  `UNPIN`, `RELEASE`) that returns the instance to the prior state up to
  performance.
- **R3 Budgeted and expiring.** Every residency or structure request
  carries a budget (memory, build time) and an expiry. An unrenewed request
  lapses. The agent cannot hold instance resources by silence.
- **R4 Agent role.** The agent's grant set is a subset of a user's. What
  it adds is nothing; what it removes is stated in §4.
- **R5 Observable by SQL.** Hot keys, miss cost, residency state, Cabin
  population and serve/fall-through counts are exposed as relations the
  agent queries. Observe-by-SQL and act-by-SQL close the loop without any
  engine hook.
- **R6 Non-destructive under error.** `[operator]` A wrong optimization
  statement must not cause a destructive outcome. The worst case of any
  agent action is a bounded performance loss, never loss of data,
  correctness, durability, or availability.

## 4. R6 elaborated — what "destructive" means and how each is excluded

`[design]` An agent will be wrong: predictions miss, models drift, prompts
are adversarial. R6 requires that the surface be designed so that being
wrong is cheap. "Destructive" is enumerated, not left to judgment.

| class | what a wrong statement could do | exclusion |
|---|---|---|
| D-A correctness | serve a stale or partial structure as authoritative | R1: the serve path serves only witnessed-complete sets; advisory sets prefetch and probe, never answer alone (`[source-read]` `docs/spec/cabin.md:50` — Waystone "never authoritative", zero write cost, droppable wholesale) |
| D-B data loss | issue `DROP TABLE`, `TRUNCATE`, `DELETE`, a narrowing `ALTER`, or drop a **Bound** Cabin whose loss fails statements | the agent role's grant set **excludes every statement that discards user data or a durable structure**; the agent may drop only what it created and only what is Observational or advisory |
| D-C availability | a build that holds a latch across a park, a full-coverage build on a large relation, a residency request that evicts the working set | R3 budgets bound memory and build time; builds are lazy/observational by default; **the agent may not create Bound Cabins** (eager, pinned, `eviction: forbidden`, `failure: fail the statement` per AR1 §9 table) — those remain a human DDL decision |
| D-D durability | an unlogged structure the recovery path expects | agent-created structures are unlogged and rebuilt by traffic; nothing the agent creates is on the recovery path |
| D-E irreversibility | a change with no inverse | R2: no statement enters the agent's grant set without an inverse |
| D-F privilege escalation | the agent grants itself, or another role, more | the agent role cannot `GRANT`; its grant set is closed under its own actions |
| D-G runaway | a loop of create/drop or pin/unpin that thrashes the pool | rate limit per role (value provisional, Guideline 2) and a circuit breaker: when the agent's aggregate footprint exceeds its budget the instance lapses **all** the agent's requests wholesale, which is a legal operation for every structure it may own (D-A, D-C) |

Two design rules fall out:

- **Rule R6-1: the agent's lever set is the set of operations that are
  wholesale-droppable.** If dropping a thing without notice could fail a
  statement or lose data, the agent does not get the lever. This is the
  same line AR1 §9 draws between Observational/advisory and Bound.
- **Rule R6-2: every optimization statement supports `EXPLAIN` (dry-run)**
  returning its projected footprint against the role's remaining budget,
  so an agent can check before acting and an operator can audit intent,
  not just effect.

R6 is why the concept survives contact with a bad agent. Without it the
concept is "give a model a superuser connection", which is the failure
mode the industry already has.

## 5. Placement against existing ckdbs structures

`[design]` The concept needs almost no new structure; it needs the
existing ones to be reachable by statement, budgeted, and role-gated.

| lever | existing structure | what the agent would issue | gap |
|---|---|---|---|
| read-assist over a known key set | Observational Cabin — key function `F`, identity `(rel_oid, expr_id)`, lazy population of observed keys, servable by cover (AR1-4, AR1-5, §9.2) | `CREATE CABIN ... ON rel(F)` then **touch the keys** so they become observed | a way to observe keys without a real read (a `PREPARE`/`WARM` verb), or accept that the agent's own `SELECT` is the observation |
| residency of a tuple set | buffer pool residency (AR0 shared pool) | `WARM rel WHERE id IN (...)` with budget/expiry | no residency-priority verb exists; the pin must be a priority, not a borrow (AR2: never a lock, never a latch) |
| advisory location | Waystone trail, `(fetch_id, arg)` (AR1 §4, §9.1) | populated as a side effect of the agent's probes | none beyond what AR1 proposes |
| observation | `sys.cabins`, `sys.patterns`, statement statistics | `SELECT` | miss-cost and residency relations do not exist |
| authorization | role model | `CREATE ROLE agent ...` | grant set closed per §4 |

Nothing in this table is built by this note. Each gap becomes a work-order
item only after §7's measurement says the concept has a consumer.

## 6. Where it applies — market scope

`[design]` Value is proportional to how far ahead of an access its key is
knowable. Ranked by that:

- strong: scheduled reports and batch windows; event-driven pipelines
  where the event names the key; session-scoped load (game sessions,
  feature serving for a known user set); time-of-day repetition
- moderate: OLTP with a skewed and slowly drifting hot set
- weak: uniform-random OLTP; ad-hoc analytics with unpredictable keys

The initial segment is the first row. A concept that claims the third row
will not measure well and should not be claimed.

## 7. What this note does not license

Guideline discipline, restated for this item:

- No CIP, no work order, no structure until a **diagnostic counter** has
  measured, on existing benchmarks under the AS-E epoch and after the
  AM-S6 baseline exists: the fraction of accesses whose key was knowable a
  fixed window ahead, and the summed miss cost of those accesses. That
  sum is the ceiling an oracle agent could recover. If the ceiling is
  small, the concept is closed regardless of agent quality.
- No new stage opens while S1b/S3/S4 are half-landed (AW-M1 close).
- No budget, rate, or threshold value is written anywhere without a
  measured or explicitly provisional tag.

## 8. Operator decisions this note leaves open

1. Whether the note is kept in-tree, and where (`docs/concepts/` proposed).
   **Answered 2026-09-08: kept, at `docs/blueprint/`.**
2. Whether the agent's key observation is a dedicated verb or its own
   `SELECT` (§5 row 1).
3. Whether residency priority is a lever at all in v3.0.0, given AR0's
   pool and eviction work.
4. Whether R6-1's line (agent may own only wholesale-droppable structures)
   is ratified as the governing rule for the agent role.
5. The circuit breaker of D-G: exists / does not exist. Its value is not
   asked.

---

## CLA's filing note — 2026-09-08, on `docs-cn1-concept-note` from `2c40c51`

The body above is as the operator supplied it. Two things this filing
adds, both marked in place: §8 item 1 and the header's location line are
answered, since the operator's instruction to file the note under
`docs/blueprint/` **is** that answer, and a note kept at one path while
proposing another would misread a week later.

**Both `[source-read]` citations verified at this commit**, which is the
one thing a filing owes a note that carries them:

- `docs/spec/cabin.md:50` is exactly the Waystone row of the three-class
  table — *never authoritative (advisory)*, *zero* write cost, droppable
  *wholesale*. D-A's parenthesis is accurate to the line and to the
  column headings. (AR1 §9.1 cites the same row as `cabin.md:47`; the row
  has moved since, so a later reader following the older citation lands
  three lines high. Not this note's error.)
- AR1 §9's class table gives Bound: *eager*, full coverage *pinned* at
  `CREATE`, eviction **forbidden**, failure **fail the statement**. D-C's
  parenthesis is accurate to all four, and R6-1's "same line AR1 §9 draws"
  is the boundary that table's first two columns describe.

**What §7 gates on is not yet reachable, and the note is right that it is
not.** The AM-S6 baseline it names does not exist: the scenario matrix is
being re-based on `f6ed10c` under BTREE-only drivers (the operator's AS-Q6
mark of the same day, `instructions/v3.0.0/raft-marks-2026-09-08-as-q6.md`),
and until those files land there is no comparator for scenario0 or
scenario2 at all. The half-landed AW-M1 stages §7 also names are S3's
instance-wide half, S4's remainder and S5.
