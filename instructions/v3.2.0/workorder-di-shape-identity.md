# Work order DI - a shape's statistics named by what the statement binds: `sys.statement_stats` keyed by the relations a statement resolved to, and a plan that matches nothing said so, the census before anything is built

Status: **OPENED 2026-11-20 on the operator's standing go-ahead**
(go-ahead-achieving-milestone, `raft-marks-2026-11-20.md` §6), on worktree
`di-shape-identity` from `1c7bb774` (`v3.4.15-4-g1c7bb774`). Every item the
operator had not marked is **adopted as CLA proposed under that word, by
CLA and not as the operator's own mark**: DI-Q0 (a); **DI-Q3 `[quiet-wrong]`
(a) as revised by DI-S0** (§7); DI-Q6..Q13 as their stages reach them, each
recorded in §4 and §7. DI-S0's census and cells are committed (`92a022f8`);
its measurement and its review's corrections follow in §7. **DI-S1 landed**
(§7): a `sys.statement_stats` row is one binding's. **DI-S2 landed** (§7):
the Cabin controller's signal is a fingerprint's and its candidate's.
**DI-S3 landed** (§7): a Waystone opt-in is a shape under what its example
bound, superblock 38, DA-S3 in it. **DI-S4 landed** (§7): a plan a rename
put out of reach reads `invalid` and restricts no drop.

Written in the main checkout on
`main` at `f4138860` (`v3.4.15-1-gf4138860`) on the operator's words of
2026-11-20 (`raft-marks-2026-11-20.md` §3). The letter DI is CLA's proposal.
**DI-Q1, DI-Q2, DI-Q4 and DI-Q5 are marked (a) by the operator's word "as
proposed"**; DI-Q3 was proposed in the conversation in a form the tree does
not support and is back with the operator (§0, §4); every other item is
CLA's proposal, unmarked. **Widened the same day on the operator's second
word** (`raft-marks-2026-11-20.md` §4, §0a below): DI covers every structure
keyed by a statement's text alone, so **DI-Q4 is re-marked (b)** - the
Waystone opt-in is in DI - and the Cabin controller's signals, read at
`5915182c`, are in it too.

Every `[source-read]` claim of §0 to §2 was read at `f4138860` (§0a at
`5915182c`); §7 carries what each stage read and at which commit.

Tags: `[operator]` the operator's word; `[source-read]` read in the tree,
with `path:line`; `[measured]` with the results file; `[design]` CLA's,
unbuilt and unmeasured; `[quiet-wrong]` where a wrong choice turns a
correct reading into a wrong one with no error.

Related: `docs/spec/sys-views.md` §6.1, §6.1a (`sys.statement_stats`,
`sys.statement_histogram`), §6.7 (ASH); `docs/spec/parser-v2.md` I12 (the
captured order's key, an administrator's plan); `docs/spec/namespace.md`
NS2, NS5, NS6, NS8; `docs/spec/waystone-concpets.md` §3, §4;
`docs/inflight/known-gaps.md` ("Captured plans (CX)", "An opt-in is by
text"); `instructions/v3.2.0/workorder-cx-managed-plans.md` (CX-R3, CX-R11,
CX-Q16); `instructions/v3.2.0/workorder-df-statistics-coverage.md` (DF-R3,
DF-R6, DF-R7); `instructions/v3.0.0/workorder-bo-sys-statistics-show-sugar.md`
(BO-Q6, BO-Q7); `instructions/v3.2.0/workorder-da-access-stats-memory-resident.md`
(the close cells DI's close repeats).

## 0. What DI is

`[operator]` *"First, it seems the priority is to tie the parsing of a query
and the statistics about a query to the oid and the namespace, so that
there is no quiet wrong answer."* and *"Write the DI work order as
proposed."* (given in Korean; recorded in English.)

The words follow a conversation in which CLA compared the engine with a
shared cursor cache and a query store, and named the identity of a
statement shape as the first thing to settle before any statistics history
or per-plan statistics is built on it.

`[source-read]` **What is and is not wrong today.** A fetch id is a hash of
the statement's text and holds no namespace and no oid
(`docs/spec/waystone-concpets.md:161`). Three structures keyed by it
already carry the relations beside it, compared whole:

- a captured order and an administrator's plan: `exec::CapturedKey` -
  fetch id, relation count, every oid in written order
  (`include/kds/exec/captured_plans.hpp:70-74`);
- an `ADVISE PLAN`'s advice: the same key
  (`include/kds/exec/plan_advice.hpp:32`, `:70`);
- a join-subset posterior: `stats::PlanKey`, "Matched exactly, never by
  hash alone: a fetch id hashes names, not oids, so `USE ns` or a DROP and
  re-CREATE reach new keys" (`include/kds/stats/plan_key.hpp:22-29`).

A Waystone replay checks each entry's `rel_oid` against the query's step
(`waystone-concpets.md` §2, rule 1). **CLA found no path on which a
statement's rows are wrong because of the fetch id's text identity**, and
the conversation said so before the word was given.

What is wrong is a number. `sys.statement_stats` and
`sys.statement_histogram` are keyed by `(fetch_id, kind)` and nothing else
(`include/kds/stats/statement_stats.hpp:23`;
`src/stats/statement_stats.cpp:46-51`, `:58-66`; the one call,
`src/server/command_dispatcher.cpp:1475`). So one row sums:

- the same text run under two namespaces (`USE a` and `USE b`, NS5 and
  NS8: an unqualified name is the session's namespace only);
- the same text over a relation dropped and created again under its name.

The row carries no mark that it did. `plan_source` and `plan_name` on that
row are "the latest compile's, on the core that saw the shape last"
(`docs/spec/sys-views.md` §6.1), so a row can name one binding's plan
beside another binding's costs. `sys-views.md` §6.1 calls the view "how a
DBA or an agent finds a shape that runs uncaptured": **a reader who acts on
the row acts on a sum of two statements** `[quiet-wrong]`, in the sense
this order uses the tag - a wrong reading with no error, not a wrong
result.

`[design]` DI makes a statement shape's statistics name what the statement
bound, and makes a plan that can match no statement say so. It changes no
result, no plan choice, no fetch id and no wire frame.

**What CLA told the operator, corrected.** CLA proposed the statistics
identity as "the plan key, reused" (DI-Q3 as spoken). Read against the
tree, `CapturedKey` exists only for a block the join search orders: "NULL
is a block the join planner did not order and no plan chose: a statement
over one relation under every `join_order`" (`sys-views.md` §6.1), and an
`INSERT`, `UPDATE` or `DELETE` has none. A key that exists for joins alone
leaves every one-relation statement merged as today. DI-Q3 is restated in
§4 and is not covered by "as proposed".

## 0a. The widening: every path, not one view

`[operator]` *"Is there no such thing outside `statement_stats`? It has to
be applied to every path on which this problem can occur."* (given in
Korean; recorded in English.)

`[source-read]` Read in the main checkout on `main` at `5915182c`. There is
one more path, and on it **the engine itself acts on the merged number**:

- The Cabin controller's execution signal is keyed by `pattern_id` alone
  and keeps one "last-observed candidacy" `(rel_oid, col_pos)` per
  pattern, on the stated premise that "a shape's target is a function of
  the shape, so overwriting is idempotent in the steady state"
  (`include/kds/stats/optimizer_signals.hpp:87-97`). The dispatcher
  derives the candidate from the compiled chain of the statement that ran
  (`src/server/command_dispatcher.cpp:10712-10720`), and `NoteExecution`
  overwrites it (`src/stats/optimizer_signals.cpp:68-72`).
- The premise does not hold for one text under two namespaces, or over a
  relation dropped and created again: the pattern is one, the relation is
  two. The decayed executions and pages of both are one sum, and the
  controller credits the whole sum to the candidate seen last
  (`src/stats/cabin_optimizer.cpp:239-240`), so a `CREATE`, an extend or
  a drop of a `CABIN AUTO` Cabin on one relation is priced with another
  relation's heat.
- **No result is wrong by it** (a Cabin is authoritative for what it
  observed, `cabin.md` C1, and a controller-made Cabin is read only
  through a plan that names it, CX-S3b), and the controller is off by
  default (`docs/spec/cabin-controller.md:6`). What is wrong is a build or
  a drop the workload did not earn `[quiet-wrong]` in this order's sense.

So the paths, as read:

| path | keyed by | who acts on the merge | in DI |
|---|---|---|---|
| `sys.statement_stats`, `sys.statement_histogram` | `(fetch_id, kind)` | a DBA or an agent | DI-S1 |
| the Cabin controller's signals, and the ASH wait sample folded into them (`src/sched/ash_sampler.cpp:150`) | `pattern_id` | **the engine's controller** | DI-S2 |
| the Waystone opt-in (`sys.patterns`, each core's opted map) | `fetch_id` | the recorder: a shape nobody opted for that relation records | DI-S3 |
| a Waystone trail | `(fetch_id, arg_hash)` (`include/kds/stats/instance_key.hpp:38`) | replay checks `rel_oid`, so no row is wrong; whether two bindings overwrite each other's trail was **not read** | DI-S0 reads, DI-S3 |
| the session's published statement, `sys.ash`, `sys.session_state` | the fetch id and `pattern_id` | a reader joining a sample to a statistics row | DI-Q9 |

Read and found keyed by relation or by the binding already, so outside DI:
captured and administrator's plans, `sys.plan_advice`, the join-subset
posteriors and the order memo (`include/kds/exec/plan_memo.hpp:35-37`: its
key holds `stats::PlanKey`), `sys.access_stats`, `sys.relation_stats`,
`sys.indexes`' usage, `sys.refusal_stats` (`(core, code)`,
`include/kds/stats/refusal_stats.hpp:18`).

**Not a complete census.** The list is every file under `include/kds` and
`src` that names `fetch_id` or `pattern_id` (40 files by `grep`), of which
the rows above were opened; DI-S0 opens the rest and adds what else names a
statement by its text - a prepared statement held across a `USE` first.

## 1. The tree as read `[source-read]`

### 1.1 Who is keyed by the fetch id alone

| structure | key | what a merge of two bindings makes of it |
|---|---|---|
| `sys.statement_stats`, `sys.statement_histogram` | `(fetch_id, kind)` (`statement_stats.hpp:23`) | costs, counts and the latency distribution summed; `plan_source` of one beside the costs of both |
| the session's published statement, and ASH | `fingerprint` = the fetch id, `arg_hash`, `pattern_id` (`include/kds/sched/wait_state.hpp:119-125`) | a sample joins to a `sys.statement_stats` row by the fetch id, so it inherits the merge; not read further |
| the Waystone opt-in | `sys.patterns` by `fetch_id` | one `WAYSTONE ON` applies under every `USE` whose text matches and survives `DROP TABLE` (`known-gaps.md:1733`); replay stays correct by the `rel_oid` check, the recording cost returns |
| the Cabin controller's signals | `pattern_id` (`include/kds/stats/optimizer_signals.hpp:110`, `:162`) | read at `5915182c`, §0a: the merged heat is credited to the candidate seen last, and the controller decides on it |

Already keyed by relation and outside DI: `sys.access_stats` (`(kind,
rel_id, column_mask)`, `include/kds/stats/access_stats.hpp:24`),
`sys.relation_stats`, `sys.indexes`' usage.

### 1.2 What a statement knows at its end

- `StatementStats::Record` runs once per statement at its end, on the
  statement's own core, under that core's part latch, and allocates only on
  a shape's first statement (`statement_stats.cpp:58-75`;
  `statement_stats.hpp:30-41`). A part holds 4,096 shapes
  (`statement_stats.hpp:119`), a new shape past it is dropped and counted.
- "Every relation a statement names is declared at the bind and held for
  the statement" (`CLAUDE.md`, the Transactions row; `docs/spec/txn.md`
  §5), so the relations are known before a row is read. **Where the
  dispatcher keeps them for the statement's end, and whether a statement
  refused before its bind has any, was not read**; DI-S0 reads it.
- `kMaxPlannedRelations` is 16 (`include/kds/exec/step_chain.hpp:958`),
  the width of `CapturedKey::oids`; `kMaxPosteriorRelations` is 4
  (`plan_key.hpp:17`).

### 1.3 A plan that matches nothing

- **A rename.** "A fetch id hashes names, so after `ALTER TABLE ... RENAME
  TO` or `RENAME COLUMN` of a name the statement writes, no statement in
  the new spelling matches the plan; `sys.plans` still shows it `enabled`
  and it still RESTRICTs the relation's `DROP TABLE`"
  (`known-gaps.md:2395-2402`, verified at CX-S1b by reading).
- **A captured order that stops compiling** is declined statement by
  statement and counted, "its row unchanged"; `INVALID` (CX-Q16) is not
  built for a captured row (`known-gaps.md:2255-2261`).
- **A dropped relation's row** is removed in the step after the drop
  commits, or at the next mount, and is inert between: "an oid is never
  reissued" (`known-gaps.md:2419-2430`).
- **A fingerprint version bump**: a captured row of another version is
  removed at the mount (`include/kds/exec/plan_catalog.hpp:56-62`;
  `src/exec/plan_catalog.cpp:596-601`). What a mount does with an
  administrator's row of another version has no cell ("no second version
  exists to write one", `known-gaps.md`, "What CX-S1b's cells do not
  reach") and was not read.

### 1.4 What is not measured

No results file prices `StatementStats::Record` by its key's width. The
nearest are BO's close (the recording of the two BO stores, 0.35-0.52 us a
point statement, inside the largest same-binary gap; cited by DA §1.1) and
DF's close, which left its 0.5 us bar unresolved on a point read
(`raft-marks-2026-11-19.md` §10). **Any cost DI adds lands on a path whose
last measurement did not resolve its own bar.**

## 2. The candidates `[design]`

None is built or measured.

**The identity** (DI-Q1, DI-Q3):

- **I-a - the fetch id stays; the statistics key gains the binding.** A
  shape's row is `(fetch_id, kind, the relations the statement bound)`.
  The fetch id, `C_PARSE`'s answer, `sys.plans` and
  `kFingerprintVersion` do not move; `sys.patterns` moves only by DI-Q13,
  since the widening. **Marked** (DI-Q1 (a)).
- **I-b - the fetch id itself carries the binding.** Owes a fingerprint
  version bump that lapses every `sys.patterns` and `sys.plans` row, a
  fingerprint that needs a catalog (today it is taken at parse:
  `docs/spec/protocol.md` §5, `C_PARSE`), and `WAYSTONE OFF <select>`,
  which "only parses and fingerprints, never compiles" so that it works
  after its relations are gone (`waystone-concpets.md` §4a). Not taken.

**What "the binding" is** (DI-Q3), under I-a:

- **B1 - the statement's bound relations, by oid, in written order**, the
  same list for a join, a one-relation read and a write; for a searched
  join block it equals `CapturedKey`'s oids, so a plan's row and its
  statistics row are found by one value.
- **B2 - the first relation's oid only.** One word; tells `USE a` from
  `USE b` and a re-created relation for every statement whose first
  relation differs, and merges two joins that differ in a later relation.
- **B3 - a hash of B1's list.** One word; a collision merges two bindings
  silently, which is the defect DI exists to remove `[quiet-wrong]`.

The namespace is not a key under any of them: an oid names one relation of
one namespace for its life (NS2). It is a column the view resolves.

**A statement with no binding** (DI-Q2): refused before its bind, or
naming no relation. One row per `(fetch_id, kind)` as today, with the
binding empty and the view saying so. **Marked** (a).

**A plan that matches nothing** (DI-Q5, marked (a) for the rename;
DI-Q10 for the rest):

- **P1 - a state derived when `sys.plans` is read**: the row's statement
  is parsed and resolved again at the read, and a row whose text no longer
  resolves to its recorded oids reads another state than `enabled`.
  Nothing is persisted, so no row format moves.
- **P2 - a state written by the DDL that orphans the plan** (the rename
  rewrites the row). Exact at the moment of the DDL, and owes a logged
  write inside `ALTER TABLE` and a row format decision.

## 3. Rules

Binding once DI opens.

- **DI-R1 - no result moves.** No statement's rows, refusal, plan or
  `plan_source` changes; the fetch id and `kFingerprintVersion` do not
  move (I-a).
- **DI-R2 - a row is one binding's** `[quiet-wrong]`. No
  `sys.statement_stats` or `sys.statement_histogram` row sums two
  statements that resolved to different relations; a row whose binding
  the engine did not know says so and is never shown as a binding's.
- **DI-R2a - every path, by the operator's word.** DI-R2 holds for every
  structure that names a statement by its text alone, whoever reads it -
  an administrator, an agent or the engine: none sums, credits or applies
  across two bindings. A path DI-S0 finds and DI does not fix is brought
  back to the operator by name, never left out silently.
- **DI-R3 - one identity, one name.** The binding is the value the plan
  tables already compare (`CapturedKey`'s oids) wherever both exist; DI
  adds no third spelling of "fetch id and oids" without saying why the two
  that exist do not serve.
- **DI-R4 - the statement path gains no latch, no cross-core write and no
  allocation but a shape's first.** A write never fails, delays or
  reorders the statement it describes (`statement_stats.hpp:38-41` kept).
- **DI-R5 - one core pays nothing outside the floor** (the standing
  zero-overhead rule): the `cores = 1` point cells of DA's close do not
  move outside their spread.
- **DI-R6 - nothing reads a statistic to decide** (DF-R3 kept). DI changes
  what a row means to its reader, and gives the engine no reader.
- **DI-R7 - memory stays bounded and said.** A part's bound in bytes is
  stated after the change as it is today (`sys-views.md` §6.1); a row of a
  relation that is gone is reclaimable or counted, never unbounded.
- **DI-R8 - no constant without a measurement**: a key width, a cap on
  bound relations, a reclaim threshold is DI-S0's number or is marked
  provisional where it is set.
- **DI-R9 - a plan's state is never a guess.** A row DI shows as matching
  nothing is one the engine established cannot match; a row it could not
  check reads as it does today.

## 4. Items for the operator (DI-Q)

(a) is CLA's proposal in each. **Marked by the operator's "as proposed":
DI-Q1, DI-Q2, DI-Q5; and DI-Q4, first (a), re-marked (b) by the second
word (§0a).** Every other item is unmarked.

| | item | (a) | other |
|---|---|---|---|
| DI-Q0 **adopted (a)** on the standing go-ahead | The order's standing. | **a census first**: DI-S0 builds nothing and may narrow DI-S1 and DI-S2, or cut one | (b) a build order from §2 as written |
| DI-Q1 **marked (a)** | Where the binding enters. | **I-a**: the statistics key; the fetch id does not move | (b) I-b |
| DI-Q2 **marked (a)** | A statement with no binding. | **one row per `(fetch_id, kind)`, binding empty, the view says so** | (b) not recorded |
| DI-Q3 `[quiet-wrong]` **adopted (a) as revised by DI-S0** on the standing go-ahead (§7, "DI-Q3 revised") | What the binding is. Spoken as "the plan key"; restated because the plan key exists for searched joins only (§0). | **B1**, capped at `kMaxPlannedRelations`; what a statement binding more does is DI-S0's finding, brought back before DI-S1. **Revised**: the distinct relations in first-declared order with the namespace the statement's unqualified names were resolved in; a statement over the cap is one row whose binding is not known | (b) B2; (c) B3, with a collision's merge accepted and stated; (d) as spoken: the plan key where one exists, today's row elsewhere |
| DI-Q4 **re-marked (b)** | The Waystone opt-in's reach across namespaces and a drop. | not in DI: no result is wrong, and it is a `sys.patterns` format decision - marked first, then overruled by "every path" | **(b) in DI**, the operator's |
| DI-Q12 **adopted (a)** on the standing go-ahead | The Cabin controller's signal (§0a). | **keyed by the pattern and its candidate together**, so one text over two relations is two signals and each relation's Cabin is priced by its own heat; `kMaxTrackedFingerprints` then bounds signals, not patterns | (b) keyed by DI-Q3's binding, as the statistics are; (c) a pattern whose candidate moved between two relations is not credited to either |
| DI-Q13 `[quiet-wrong]` **adopted (a) as restated at DI-S0** on the standing go-ahead | What an opt-in names once it is in DI. | **the shape under the relations `WAYSTONE ON`'s example resolved to** - it already compiles its example under the session's namespace (`waystone-concpets.md` §4a) - so a drop ends it and another namespace's same text is not opted. A `sys.patterns` row change, so a format event (DI-Q11); `WAYSTONE OFF <select>` after a drop, which today needs no relation, is restated in DI-S0 | (b) the opt-in stays by text and the manual says so, the recording cost accepted - what "every path" rules out unless the operator re-marks |
| DI-Q5 **marked (a)** | A renamed relation's plan reading `enabled`. | **in DI** | (b) left a known gap |
| DI-Q6 **adopted (a) as restated at DI-S0** on the standing go-ahead | The views' shape. The grain of a row changes from a shape to a shape under one binding. | **`sys.statement_stats` and `sys.statement_histogram` gain `relations` (oids as bound) and `namespace`; one row per binding; no second view** | (b) the old grain kept and a second view added per binding |
| DI-Q7 **adopted (a) as restated at DI-S0** on the standing go-ahead | Rows of a relation that is gone. | **kept and readable until the part is full, then the first reclaimed**; counted in `sys.meta` | (b) removed at the drop's commit on every core - a cross-core act DI-R4 does not allow on the statement path and DI-S0 would have to place; (c) kept until restart, as every row is today |
| DI-Q8 | The bar at the close. | **DA's close cells and a point `SELECT` and keyed `INSERT` at `cores` 1 and 8: no cell slower outside its own spread**; no factor proposed, since DF's 0.5 us bar on this path was not resolvable | (b) a bar the operator names |
| DI-Q9 | The session's published statement and ASH. | **left on the fetch id** unless DI-S0 finds a reader that joins a sample to a statistics row; then brought back | (b) the binding published beside the fingerprint - a word or more written at every statement's head |
| DI-Q10 **adopted (a) as restated at DI-S0** on the standing go-ahead | Which plans-that-match-nothing DI shows, beyond the rename. | **the rename and a captured order that no longer compiles (CX-Q16's `INVALID`), by P1**; a fingerprint-version row stays the mount's | (b) the rename alone; (c) P2 |
| DI-Q14 **adopted (a)** on the standing go-ahead | *Added at DI-S4.* How far a column's rename is established from a plan's row. The row stores the statement's shape text and its relations' oids, no namespace and no compiled form. | **a column the statement writes qualified by one of its bindings** (`b.col`), read off the stored text's tokens against that binding's relation; an unqualified column is not read - which relation it means is the compiler's to say - and the plan reads as it did, entered in `known-gaps.md` | (b) every column, by walking the parsed statement against its bound relations as the compiler resolves them - a second resolver to keep in step with the first; (c) the relation's rename alone |
| DI-Q15 **adopted (a)** on the standing go-ahead | *Added at DI-S4.* What a plan that can match nothing reads, and what `DROP TABLE`'s RESTRICT does with it. | **`invalid`, for either origin** - the word an administrator's row already reads where it can serve no statement - **and it restricts no drop**: it is in force for no statement, as a disabled plan is, and a rename back puts it in force and RESTRICT with it | (b) a word of its own, and RESTRICT as before until `DROP PLAN` or `ALTER PLAN ... DISABLE` |
| DI-Q11 **adopted (a)** on the standing go-ahead | A format event. | **one, for DI-Q13 alone**: the statistics and the controller's signals are memory-resident and P1 persists nothing. The bump is the host DA-S3 waits for | (b) P2 admitted too |

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **DI-S0 - census and premise** | No engine code. **Read**: every structure keyed by the fetch id or the pattern id, with or without the relations beside it, and what reads it (§1.1's table completed, the Cabin controller's row first); where a statement's bound relations live at its end, for each statement kind, a subquery's included, and what a statement refused before its bind has; every DDL that changes what a text resolves to - `USE`, `DROP` and re-`CREATE`, `RENAME TO`, `RENAME COLUMN`, `DROP NAMESPACE`, `REINDEX`, `ADD`/`DROP COLUMN` - against every keyed structure, each cell answered by a test that exists or by a reading at a cited line; a mount's handling of an administrator's row of another fingerprint version. **Show**: the merge of §0, as two cells against a server - two namespaces, and a drop and re-create - each ending in one row. **Measure**, `build-release`, inside the stage budget: `Record`'s share of a point `SELECT` at `cores` 1 and 8 today, and the same with B1's and B2's key in a spike that is never committed, its binary hashed. | the completed table, the results file, each DI-Q restated against what was read, and the unmarked ones back to the operator |
| **DI-S1 - the statistics' identity** | Under the marks: the key, the two views' columns, the empty binding's row, DI-Q7's rows. `sys-views.md` §6.1, §6.1a. | review, the full suite |
| **DI-S2 - the Cabin controller's signal** | Under DI-Q12: the signal's key, the snapshot, `Decide`'s sum; `optimizer_signals.hpp`'s premise restated. `cabin-controller.md`. | review, the full suite |
| **DI-S3 - the Waystone opt-in and the trail** | Under DI-Q13 and what DI-S0 read of the trail: `sys.patterns`, the opted map, `WAYSTONE ON`/`OFF`, the mount's renewal; the superblock bump, with DA-S3 in it. `waystone-concpets.md` §4-§4c, `known-gaps.md`. | review, the full suite, `scripts/sim.sh` |
| **DI-S4 - a plan that matches nothing** | Under DI-Q5 and DI-Q10: the state in `sys.plans`, and what `DROP TABLE`'s RESTRICT does with a plan that can match nothing. `parser-v2.md` I12, `known-gaps.md`'s two entries. | review, the full suite |
| **DI-S5 - the close** | DI-Q8's cells, and CF-S2's opted and not-opted arms, interleaved A/B, the commit DI opened from against the closing code; the documents; the milestone row. | closed |

## 6. What DI does not do

- No change to the fetch id, the pattern id, `kFingerprintVersion` or a
  KWP frame.
- No history: statistics stay memory-resident and emptied by a restart. A
  persisted, interval-bucketed history and statistics per plan variant
  were discussed with the operator and are a later order's; DI is what
  they would be keyed by.
- No compiled-chain or parse cache (CX-S6's ruling stands).
- No statistic read by the engine (DI-R6).

## 7. Stage log (CLA, appended as stages land)

### Written 2026-11-20

In the main checkout on `main` at `f4138860`, documents only. The order
rests on a reading of the sites in §0 and §1; nothing was run. The claims
of §1.3 quoted from `known-gaps.md` are that file's, verified where it
says, and were not re-read against the source here. Open with the
operator: DI-Q0, DI-Q3 and DI-Q6..Q11, and whether DI opens.

### Widened 2026-11-20

In the main checkout on `main` at `5915182c`, documents only, on the
operator's second word (`raft-marks-2026-11-20.md` §4). §0a added from a
reading of the controller's signal path; DI-R2a, DI-Q12 and DI-Q13 added;
DI-Q4 re-marked (b); the stages renumbered (the controller's signal S2, the
Waystone opt-in S3, the plan's state S4, the close S5). Nothing was run.
Open with the operator: DI-Q0, DI-Q3, DI-Q6..Q13 but DI-Q4, and whether DI
opens.

### DI-S0 - census and premise, 2026-11-20

On worktree `di-shape-identity`, opened from `1c7bb774`
(`v3.4.15-4-g1c7bb774`); every line below was read there. No engine code
is committed by the stage: two cells (`tests/shape_identity_test.cpp`) and
documents.

**The merge, shown.** `ShapeIdentityTest`'s two cells run one text under
`USE east` and `USE west` (one row answered, then three), and one `INSERT`
before and after a `DROP TABLE` and a `CREATE TABLE` of its name, and each
ends in **one** row of the store `sys.statement_stats` reads
(`StatementStats::Snapshot`) with `calls = 2`. They are green at `1c7bb774`
and are the cells DI-S1 turns over. They drive a dispatcher with no lock
table and no transaction manager; DI-S1 adds the cell on the two-core rig.

**The census, completed.** `grep` finds 40 files under `include/kds` and
`src` naming `fetch_id` or `pattern_id`; every one was opened - by CLA, or
by one of the stage's two read-only census agents, whose decisive lines
CLA read again (`trail_replay.cpp:38`, `optimizer_signals.cpp:72`,
`plan_catalog.cpp:548-555` and `:583-601`, `kwp_session.cpp:697-701`).
Keyed by the text alone, with what a merge of two bindings makes of it:

| structure | key | read at | a merge of two bindings |
|---|---|---|---|
| `sys.statement_stats`, `sys.statement_histogram` | `(fetch_id, kind)` | `statement_stats.hpp:145-149`, `statement_stats.cpp:68` | one row, the sum; shown by the two cells |
| the Cabin controller's signal | `pattern_id` (`optimizer_signals.hpp:215`) | `optimizer_signals.cpp:68-72`, `cabin_optimizer.cpp:216-259` | executions and pages of both in one entry, the candidate overwritten by whichever ran last with a candidacy, and `Decide` credits the whole sum to it: every CREATE, EXTEND and DROP reads that sum. **The execution feed runs whether the controller is on or off**, for every statement that succeeded with an identity (`RecordExecution` to `RecordOptimizerSignals`, which checks only that `optimizer_signals_` is set), under its latch above one core; the controller's switch gates `Tick` and the wait samples |
| the controller's wait samples | `pattern_id` (`optimizer_signals.cpp:83-89`, from `ash_sampler.cpp:148-153`) | the same entry | "read by nothing that decides" (`optimizer_signals.hpp`) |
| the Waystone opt-in | `fetch_id`: `sys.patterns`' row, each core's opted map (`catalog_cache.hpp:278`), `PatternAccess` holding no relation (`schema.hpp:583-600`) | `catalog.cpp:3051-3123` (the lookups), `SetPatternWaystone` and `RenewPatternDirectories` (the only writers), the gate at `command_dispatcher.cpp`'s `FindOptedPattern(fingerprint->fetch_id)` | one `WAYSTONE ON` opts the text in under every namespace; ON's refusals (D2, no descent) were checked for the namespace it ran under only; no `sys.patterns` writer runs on a drop or a rename |
| a Waystone trail | the row's directory by `fetch_id`, a page by `arg_hash` (`instance_key.hpp:37-45`, `waystone_dir.hpp:15-16`) | `trail_store.cpp:59`, `:87`, `:129`; `trail_replay.cpp:24-53` | **two bindings take turns overwriting one page**: each run reads the other's trail, `TrailReplay::Build` drops every entry whose `rel_oid` is not the step's (`trail_replay.cpp:38`, counted nowhere), the statement descends, and the recorder finds the trail "changed" and rewrites it. No row is wrong: the `rel_oid` check is what tells the two apart (both are one `InstanceKey`, so the page's identity does not), and a replayed tuple is verified against its page besides. The replay is lost and the page dirtied at every run. **This is the cell §0a left unread** |
| the recorder's sightings | `InstanceKey` (`trail_recorder.hpp:145`) | `trail_recorder.cpp:69-124` | one count for both bindings |
| the session's published statement, `sys.ash` | the fetch id (`wait_state.hpp:119-127`; `sys.ash.fingerprint`) | `command_dispatcher.cpp`'s `DispatchAsync` head; `sys_views.cpp`'s `sys.ash` | **no engine code joins a sample to a statistics row**; `tools/ash_panel.sql` and `.py` tally samples by `fingerprint` and read no statistics view. `sys.session_state` carries no fingerprint |

Keyed with the relations beside the text, so outside DI, as §0a said and
now read whole: `exec::CapturedKey` (captured and administrator's plans,
`sys.plans`, `sys.plan_advice`), `stats::PlanKey` and the order memo
(`plan_stats.cpp:145-146` mixes the oids). **A prepared statement holds its
text, its pattern id to echo, and nothing compiled** (`kwp_session.hpp:285-289`; `C_EXECUTE`
dispatches `portal->sql`, `kwp_session.cpp:697-701`), so `PARSE`, `USE`,
`EXECUTE` runs under the namespace at the execute and no structure is keyed
by it. The other files name the ids as inputs or in comments only.

**Every DDL that changes what a text resolves to**, against the four
structures DI changes (a reading at the lines above unless a test is named):

| | statistics row | controller's signal | opt-in | trail |
|---|---|---|---|---|
| `USE` | merged (`ShapeIdentityTest`) | merged, credited to the candidate seen last | applies to both | overwritten in turn |
| `DROP` and re-`CREATE` | merged (`ShapeIdentityTest`) | the new relation inherits the old one's decayed heat | survives the drop and applies to the new relation (`known-gaps.md`, "An opt-in is by text") | the old entries drop at `Build`, then re-recorded |
| `RENAME TO`, `RENAME COLUMN` of a written name | the new spelling is another fetch id: a new row, the same relations | another pattern; `Decide` sums by candidate, so the relation keeps its heat | the old text no longer compiles; the row is inert | inert |
| `DROP NAMESPACE` | RESTRICT: its relations are dropped first, so `DROP`'s row | as `DROP` | as `DROP` | as `DROP` |
| `REINDEX` | none: no structure keyed by a fetch id holds an index oid (the trail entry's, the candidate's and the binding's fields) | none | none | none |
| `ADD COLUMN`, `DROP COLUMN` | none: the text and the relation are the same | see "found and not DI's" | none | entries verify by page epoch, unchanged |

**Where a statement's relations are at its end** (DI-S0's second reading).
Nowhere, today: every fingerprinted statement declares each relation it
binds to a `ReadBorrow` (`read_borrow.hpp`), whose record dies with the
borrow and is not kept at all where the dispatcher has no lock table.
But every bind of every statement kind passes one of six borrows, all made
by the dispatcher inside the statement's synchronous body:

| kind | where its binds are declared |
|---|---|
| a native `SELECT` | the run's borrow (`SelectBody`), by the compiler's one bind loop for `FROM`, every `JOIN` and every subquery block (`step_compiler.cpp`, "One site for FROM, every JOIN and every subquery block") - before the walk, so before the run can park |
| a query layer `SELECT` | `LayerHostImpl`'s one borrow, through which every `DescribeRelation` compiles its probe - once per relation source, at the layer's compile, in written order (`query_layer.cpp`, `Compile`). A walk is a `SelectBody` of its own with a borrow of its own, run to its end inline (`WalkForLayer`'s `RunToEnd`); **walks are lazy and cached, so which run and in what order follows the data**, and the binding is the describes' alone |
| `INSERT`, `UPDATE`, `DELETE`, `SELECT ... FOR UPDATE` | the target at its resolve; a `WHERE`'s subquery relations through `CompileWhere` into the same borrow; a `SET`'s scalar subquery and an `INSERT ... SELECT`'s source through the `SELECT` path |
| a statement refused before its bind | nothing is declared; its fetch id is set at the parse, so it is a row with a fetch id and no binding |

A re-run (`DispatchAndStage` again: a structural refusal, a page wait, a
`PURGE`'s wait) begins a new body and declares afresh. `ResolveNames` can
make one resolve twice when a DDL's generation moved under it, and the
first one's declarations stay in the borrow: **the binding must be the
resolution that answered, so what the first resolve declared is forgotten
before the second** (the review's finding 1: a rename swap between the two
would otherwise leave one list for two resolutions). A reply
rebuilt over an outcome (`SelectRun::Execute`'s `Finish`, a refused commit)
keeps `plan_source` and `compile_ns` by copying them across; the binding
takes the same path.

**DI-Q3 revised** `[quiet-wrong]`. B1 as written - the bound relations "in
written order", a reference at a time, as `CapturedKey` holds a searched
block's - is not a function of the statement: a compile can run twice, a
query layer statement binds a relation once per walk, and a write binds its
target and its subqueries through separate compiles. What is a function of
it is **the distinct relations in first-declared order**. That alone is not
exact: `a.t JOIN b.t JOIN t` under `USE a` and under `USE b` is one
distinct list (`a.t`, `b.t`) and two statements, which is the merge DI-R2
forbids. **With the namespace the statement's unqualified names were
resolved in, it is exact**: under one default namespace every name goes
through one map from names to relations at a time, two written names are
one relation only where they are one name, so the list's repeats fall where
the text's do, and two equal lists are two equal resolutions of every
reference. **Two premises carry that, and both are the tree's**: the
list is one resolution's (a second resolve forgets the first, above), and
the order of first declaration is a function of the text - on the native
path the binds are declared in written order before the join search
(`step_compiler.cpp`'s bind loop), a write's subqueries compile in a fixed
order, and a query layer statement's describes are its compile's; a walk's
declarations, which follow the data, are kept out. An order that followed
the data would not merely split a binding: with a rename swap it could
merge two. The namespace word is 0 for a statement that wrote no
unqualified name, so a fully qualified statement is one row whatever
namespace its session is in. The list stays ordered: a sorted set would
merge `a JOIN b` before and after the two relations swap names by three
renames. So the binding is `(namespace of the unqualified names, up to 16
distinct oids in first-declared order)`, `stats::StatementBinding`. **Past
16 distinct relations the binding is not held**: the statement is recorded
in one row per `(fetch_id, kind)` that says its binding is over the cap,
never shown as a binding's. A statement refused part way through its bind
is recorded under what it had declared by then - a prefix, never another
statement's relations. DI-R3: `CapturedKey`'s oids are a searched top-level
block's references with their repeats, and exist for that block only; the
binding is the statement's, across its blocks and compiles. For a planned
block with no self-join the two lists are the same oids in the same order,
and that is all DI promises of them. Options (b), (c) and (d) stand as
written. **Adopted, revised, on the standing go-ahead.**

**The other items, restated against what was read:**

- **DI-Q6** (a), with what "the view says so" needs: three columns on each
  view where (a) named two. `binding` (`bound`, `none`, `over`),
  `namespace` (the name of the namespace the unqualified names were
  resolved in; NULL where the statement wrote none) and `relation_oids`
  (the oids as bound, the identity a reader groups and joins by), and on
  `sys.statement_stats` a fourth, `relations`, rendered as `sys.plans`
  renders its own (`ns.name`, or `oid <n>` once the relation is gone). No
  view renders a list of oids today (`sys_views.cpp`); the names alone
  would not tell a dropped relation's row from its successor's by a join.
- **DI-Q7** (a). A part has no reader of the catalog and no drop tells a
  core's part anything, so "the first reclaimed" is placed at the one
  moment a part needs room: a new shape arriving at a full part has the
  recording core ask its own catalog which rows name a relation that is
  gone, at most once per schema version that core has seen, and reclaims
  those. No cross-core act, nothing on the path of a statement whose shape
  is known (DI-R4).
- **DI-Q9** (a) stands: no engine reader joins a sample to a statistics
  row, so the published statement stays on the fetch id. A reader outside
  the engine who joins `sys.ash.fingerprint` to `sys.statement_stats` gets
  every binding's rows of that text; `sys-views.md` §6.7 is to say so
  (DI-S1).
- **DI-Q10** (a), restated: **P1 as written cannot be done from the row.**
  Neither a captured row nor an administrator's stores the namespace its
  statement was made under (`plan_catalog.cpp:246-276`, `:417-430`: the
  shape text, the oids, the bindings), so its text cannot be resolved
  again. What the row does hold is the oids. So: a renamed relation is
  established by comparing the name each written reference spells with the
  current name of the oid recorded at its position - no resolution, no
  namespace; a renamed column, and a captured order that no longer
  compiles, by what DI-S4 can establish from the stored text and the
  recorded oids, and where it cannot the row reads as it does today
  (DI-R9). An administrator's row already reads `invalid` where an oid is
  gone or its fingerprint version is another build's
  (`plan_catalog.cpp:548-555`), and **a mount leaves such a row in place,
  uninstalled, for the administrator to drop** (`plan_catalog.cpp:583-588`)
  while it removes a captured row of another version (`:598-601`) - the
  reading DI-S0 owed.
- **DI-Q12** (a) stands. A wait sample knows the pattern and no candidate
  (DI-Q9 (a)), and nothing that decides reads it: DI-S2 says where it goes.
- **DI-Q13** `[quiet-wrong]` (a): the premise holds - `WAYSTONE ON`
  compiles its example under the session's namespace
  (`command_dispatcher.cpp`, `HandleWaystone`'s `StatementContextFor`) and
  writes no relation. `sys.patterns`' row has 13 reserved bytes
  (`rows.hpp:580-687`), which hold no binding, so the row grows: the format
  event of DI-Q11. `WAYSTONE OFF <select>` restated: it resolves its
  example as ON does and clears that binding's opt-in; **a `DROP TABLE`
  ends every opt-in naming the relation**, so an example that no longer
  resolves has nothing left to clear and is answered as a shape not opted;
  `WAYSTONE OFF <fetch_id>` clears every binding of the fetch id, and is
  the form that needs no relation.
- **DI-Q8** and **DI-Q11** stand as written.

**Found and not DI's** (DI-R2a: named, not left out):

- **The controller's candidate names a column by its position**
  (`CandidateRef{rel_oid, col_pos, cabin_id}`), and nothing tells the
  signals of a `DROP COLUMN`. A position `ADD COLUMN` reuses after
  `PURGE DROPPED COLUMNS` inherits the dropped column's decayed heat until
  it decays. Not a text identity; not fixed by DI.
- `SessionWaitStats::arg_hash` has no writer, so `sys.ash.arg_hash_valid`
  is never 1.

**Measured** `[measured]`, after DI-S1 and DI-S2 were built: the run was
held 84 minutes at its quiet gate by other sessions' work on the host, and
that hold is outside the stage's budget (build about 5 minutes, run 6).
`bench/v3.4.15/results-di-s0-premise-v3.4.15-4-g1c7bb774.md`, at
`v3.4.15-4-g1c7bb774`: four binaries from one export of `1c7bb774` - A as
it is, A0 with `Record`'s call compiled out, B1 with the binding in the
key (the spike before DI-S1's review), B2 with the binding cut to one
relation - five server lifetimes an arm, a point `SELECT` at `cores` 1 and
8. **Nothing is resolved.** Server CPU a statement, mean (spread):
22.39 us (1.93), 22.74 us (0.88), 22.41 us (0.33), 22.68 us (1.08) at
`cores = 1`; 34.04 us (1.20), 33.62 us (1.05), 34.47 us (0.59),
33.80 us (0.80) at `cores = 8`. `Record`'s whole share, A - A0, is
-0.36 us and +0.42 us; B1 - A is +0.02 us and +0.43 us; B1 - B2, the
width, is -0.27 us and +0.66 us. Every delta is inside its arms' spread
and the signs disagree between the two cells. So the stage gives no ground
to narrow the key (DI-Q0 (a)), and none to say the 16-relation binding is
free: at `cores = 8` B1 was the dearer in five rounds of five, by an amount
of the order of this host's one-binary floor. **Not measured**: a write, a
binding of more than one relation, DI-S1's code as landed. DI-Q8's cells
at the close are what price it.

**The suite**: 0 failed of 4867 (2 disabled, 2 skipped), the hook's range
at `-j8`, on this commit's build inputs (`1c7bb774` and the two cells).

**The review** (`critics-developer`, of `92a022f8`; it read the tree at
`1c7bb774` and ran nothing). Eight findings, all applied, none rejected:

1. **The binding must be the final resolution's** `[quiet-wrong]`.
   `ResolveNames` runs a resolve again when a DDL's generation moved, and
   the first one's declarations stayed: three renames that swap `t` and `u`
   between the two leave `[R1, R2]` for a statement that resolved
   `t -> R2, u -> R1`. Applied to the argument above and built at DI-S1:
   the dispatcher marks the binding before a resolve and rewinds it before
   the second.
2. "An order that follows the data only splits a binding" was false: with
   a rename swap it merges. The premise is now stated as one the tree
   keeps, not as a consequence.
3. **The query layer's walks are lazy and cached** (`query_layer.cpp`,
   `SourceRows`), so which run follows the data, and each is a `SelectBody`
   with a borrow of its own - the table above had them in the host's one
   borrow. The binding is the layer's describes', and at DI-S1 a walk's
   declarations are rewound.
4. The controller's row said only `Tick` is switched; the wait samples are
   gated too, the latch exists above one core only, and the feed is every
   statement that succeeded with an identity. Corrected.
5. The index row still read DI-Q4 as the operator's (a) and DI-Q0, DI-Q3
   as unmarked. Corrected.
6. "DI-S0 landed" before its measurement overstated. Corrected.
7. The cells drive a dispatcher with no lock table; DI-S1 adds the cell on
   the two-core rig, asserts the two oids differ and each reply's rows, and
   reads the view.
8. Nits: what guards a replay across two bindings is the `rel_oid` check
   alone; the candidate is overwritten only by a candidacy; the opt-in's
   writers were not cited; a prepared statement also holds its pattern id;
   `ash_panel` tallies and does not group. The argument for DI-Q3, which
   stood in four places, is in this section alone.

### DI-S1 - the statistics' identity, 2026-11-20

On worktree `di-shape-identity`, over `f7250b89`. **DI-Q6 and DI-Q7 adopted
(a) as DI-S0 restated them, on the standing go-ahead.** The stage's premise
measurement had not run when the stage was built (the host did not go
quiet); what it says of the key's width is owed before the close.

**Built.**

- `stats::StatementBinding` (`include/kds/stats/statement_binding.hpp`):
  the namespace of the statement's unqualified names and up to 16 distinct
  relations in first-declared order; `over` past the cap; a mark and a
  rewind. 144 bytes, asserted.
- **Where it is taken.** `exec::PositionSink::Position` carries the
  namespace an unqualified name was resolved in; `ReadBorrow` tells every
  declaration to the dispatcher's `statement_binding_` before anything is
  asked of the lock table, so a dispatcher with none records the same
  binding. Cleared at a body's head, copied onto the outcome beside the
  fetch id, kept across the four sites that rebuild a reply over an
  outcome. `ResolveNames` rewinds before its second resolve; a query layer
  walk's declarations are rewound, and so are a native compile's when the
  query layer answers in its place.
- **The key**: `(fetch_id, kind, binding)`, in `StatementStats`' parts and
  in `Snapshot`'s merge and order.
- **The views** (DI-Q6): `binding`, `namespace`, `relations`,
  `relation_oids` on `sys.statement_stats`; `binding`, `namespace`,
  `relation_oids` on `sys.statement_histogram`. The lists are joined by
  `, ` as `sys.plans`' are.
- **A gone relation's rows** (DI-Q7): `StatementStats::ReclaimGone`, called
  by `RecordStatement` when a new shape meets its core's part full, at most
  once per schema version the core has seen; `statement_stats_reclaimed`
  in `sys.meta`.

**Found in the building.** A `SELECT` the native grammar does not parse
has no fetch id (`parser.hpp`: a parse that stopped in the middle answers
none), so every set operation and every other form only the query layer
reads was one row, `(0, SELECT)`, whatever it named. The binding is
recorded whatever the fetch id, so that row is now one a binding; it still
sums every such text over the same relations (`known-gaps.md`). **Not
DI's to fix and named here** (DI-R2a): giving such a statement a fetch id
of its own.

**The review** (`critics-developer`, of the working diff over `f7250b89`).
Seven findings and five simplifications:

1. **A resumed `UPDATE` lost its `SET` subqueries' relations**
   `[quiet-wrong]`: the resume reads the parked answers and runs no
   subquery, and the re-dispatch had cleared the binding, so a statement
   that parked on a row was recorded as `[t]` where the same statement
   unparked is `[t, u]`. Applied: `Session::ParkedWrite` carries the
   binding. Pinned on the rig (`BjSingleWriteRigTest`).
2. **The reclaim's gate was this core's own DDL count**, which a peer's
   drop never moves, so above one core a part swept once and never again.
   Applied: the gate is that count plus the schema word the core's cache
   was validated at. Pinned on the rig (`StatementCostRig`).
3. **A conflict arm's `SET` subquery was bound only where a row
   conflicted**, so what an `INSERT ... ON CONFLICT DO UPDATE` bound
   followed its data. Applied: the arm's subqueries are compiled once at
   the `INSERT`'s head. Pinned on the rig (`StatementCostRig`).
4. A native compile that declined for the query layer left its own binds
   under the layer's. Applied: rewound.
5. The lists were joined by a space, which a gone relation's `oid <n>`
   holds. Applied: `sys.plans`' `, `.
6. A re-run answered before its bind recorded the statement's fetch id
   beside no binding. Applied: `DispatchAsync` keeps the first run's
   binding for that case, as it keeps the fetch id.
7. What a reclaim loses for a drop that rolls back is now said in the
   header and in `known-gaps.md`.

Simplifications: the redundant alias and the repeated namespace expression
are gone. **Rejected, with the reason**: one helper for the four reply
rebuilds - they carry different fields on purpose (a refused commit keeps
`compile_ns` and drops the plan source), and unifying them would change
what DF records; the binding stored once, in the key alone - the stored
shape repeats its key today (`fetch_id`, `kind`) so that a snapshot is a
plain copy, and the 144 bytes are inside the slot bound the spec states;
a default `nullptr` for `ReadBorrow`'s binding - each site says whether its
binds are a statement's, and a default would let a new site say nothing.
The review's two cost notes (a 144-byte copy per `*out =` and per
`Record`) are the close's to price.

**Cells.** `tests/shape_identity_test.cpp` (the binding; one text under two
namespaces and over a relation dropped and created again, now two rows; a
qualified statement, one; `a.t JOIN b.t JOIN t`, two by the namespace
alone; a write and its subqueries; a query layer statement; a refusal
before the bind; a kind with no fingerprint; the views; a full part),
`tests/statement_stats_test.cpp` (two bindings merged across cores, the
overflow row, the sweep's gate), `tests/statement_cost_rig_test.cpp` (two
namespaces on two cores, a point read and a handed-on walk; a conflict
arm's subquery; a drop on another core),
`tests/bj_single_write_rig_test.cpp` (a parked statement's binding).
**Findings 1, 2 and 3 each have a mutant its cell kills**: the fix taken
out, the cell fails; put back, it passes. **No cell** for `ResolveNames`' second resolve through a
statement, or for a refused commit's rebuilt reply (`known-gaps.md`).

**The suite**: 0 failed of 4886 (2 disabled, 2 skipped), the hook's range at `-j8`, on this commit's
build inputs. Overhead not measured; measured at the milestone's close.

### DI-S2 - the Cabin controller's signal, 2026-11-20

On worktree `di-shape-identity`, over `1ffbf607`. **DI-Q12 adopted (a) on
the standing go-ahead.**

**Built.** An execution signal's key is `stats::SignalKey{pattern_id, the
candidate's relation, the candidate's column}` (`optimizer_signals.hpp`):
one fingerprint over two relations is two signals, each credited the
executions that targeted it, and an execution with no candidacy is the
fingerprint's `{0, 0}` signal, priced by nothing - before, it was added to
the frequency and the mean of the candidate the fingerprint had last
named. The Cabin serving a candidate is what the newest execution saw and
is no part of the key. `Decide` is untouched: it already summed by
candidate. **A wait sample is a fingerprint's** (DI-Q9 (a): the sampler
knows the published pattern and nothing bound): kept in `patterns_` beside
the fingerprint's signals, one entry while the fingerprint has a signal,
and reported in the snapshot's `waits`, not on a candidate's row.
`cabin-controller.md` PO7c.

**The review** (`critics-developer`, of the working diff over `1ffbf607`).
Five findings:

1. **A signal's pattern was counted after the victim was chosen**, so a
   newcomer whose pattern's only signal was the victim found the pattern's
   entry erased and its waits gone; and a second insertion that threw left
   the count one too high for good. Applied: counted first, rolled back on
   a throw. Pinned (`AVictimOfTheNewcomersPatternLeavesThePatternsWaitsStanding`).
2. **The 4,096 cap now bounds signals**, so 41 texts under 100 namespaces
   pass it, and past it every new signal evicts the coldest and pays the
   scan under the collector's latch. Not changed - the cap is `[PROPOSED]`
   and unmeasured (DI-R8) - and entered in `known-gaps.md`.
3. A candidacy that moves between two columns of one relation now leaves
   the old signal to decay where it was overwritten whole. Kept, as the
   review advised: the alternative is the old crediting of executions a
   column never served. In `known-gaps.md`.
4. `tracked_fingerprints()` counted signals: renamed `tracked_signals()`;
   the model's text in `cabin_optimizer.hpp` and `cabin-controller.md`
   §II.4 says signal where it said fingerprint. **Not renamed**:
   `kMaxTrackedFingerprints`, `FingerprintSignal`, `SnapshotFingerprint` -
   a rename across the controller, its executor and three test files for a
   word PO7c now defines.
5. The hash: no finding.

Of the three cells it named, two are added (a `Decide` cell: one pattern
over two relations, only the hot one created; the shared-pattern
eviction). **Not added**: one text over a relation dropped and created
again - the namespace cell drives the same path, two oids under one
pattern.

**Cells.** `tests/optimizer_signals_test.cpp` (one pattern over two
candidates; a candidate's Cabin moves no signal; a pattern's waits and
their leaving; the shared-pattern eviction; one text under two namespaces
through the dispatcher), `tests/cabin_optimizer_test.cpp` (the decision),
`tests/ash_sampler_test.cpp` (the sample lands in `waits`).

**The suite**: 0 failed of 4892 (2 disabled, 2 skipped), the hook's range at `-j8`, on this commit's
build inputs. Overhead not measured; measured at the milestone's close.

### DI-S3 - the Waystone opt-in and the trail, 2026-11-20

On worktree `di-shape-identity`, over `7f861cd8`, in two commits.
**DI-Q13 `[quiet-wrong]` (a) and DI-Q11 (a) adopted on the standing
go-ahead**, DI-Q13 as DI-S0 restated it; its premise was read again before
the build (`WAYSTONE ON` compiles its example under the session's
namespace and wrote no relation). **Superblock 38.**

**The format event carries DA-S3** (`78468dc8`, DA-Q1 (a)):
`sys.access_stats`' dead bootstrap is removed - oid 130 retired, page 11
reserved and unused - as that order's own last section records. Its review
found no code defect and ten stale texts, all corrected in the second
commit.

**Built** (the second commit):

- **A `sys.patterns` row is a shape under what its example bound.**
  `SysPatternRow` carries the binding - the namespace and up to 16
  relations - 41 to 178 bytes; every lookup arrives with the fetch id and
  the binding; the per-core opted map holds one entry a binding.
- **The gate asks with what the statement's own compile bound.** A
  `SELECT` run declares into its run's binding, started afresh at each
  resolve, and the statement's binding absorbs it once the compile has
  answered or been refused - which replaces DI-S1's mark and rewind around
  the native compile. A `SELECT` run inside a write is opted, or not, as
  the `SELECT` it is.
- **The recorder writes through the row the gate found**
  (`StatementIdentity::opted_row`, `Catalog::FindOptedPatternRow`) and
  counts an instance's sightings under that row. Two opted bindings of one
  text are two rows with two directories: the turn-by-turn overwrite DI-S0
  found is gone.
- **`WAYSTONE ON`** writes the binding of its example and holds its read
  borrow across the write; over more than 16 relations it is refused
  `NotImplemented`. **`WAYSTONE OFF <select>`** resolves its example and
  clears that binding's opt-in; an example that binds nothing names none
  and is answered `unchanged`, under page 9's hold like every `unchanged`.
  **`WAYSTONE OFF <fetch_id>`** clears every binding of the shape.
- **A committed `DROP TABLE` ends the opt-ins naming the relation**
  (`Catalog::EndPatternsNaming`, at the two sites a committed drop forgets
  its plans; a rolled-back drop ends nothing). **The mount ends an opt-in
  whose binding names a dropped relation** - the relation's tombstone in
  `sys.objects`, read once where any row is opted - which is what a crash
  between the drop's commit and that write leaves.
- `sys.patterns` gains `namespace`, `relation_oids`, `relations`.

**What DI-S0 owed and DI-S3 read**: `WAYSTONE OFF <select>` after a drop,
"which today needs no relation" - it has nothing to clear, since the drop
ended the opt-in; and the trail under two bindings - they took turns
overwriting one page, and now do not share one.

**The review** (`critics-developer`, of the working diff over `78468dc8`).
Three findings, two document findings and two simplifications, all
applied:

1. **A `SELECT` refused in its compile lost its partial binding**: the
   run's binding was absorbed only once the compile answered, so a join
   whose second relation does not exist was the `none` row, where DI-S1
   had it under its first relation - and two namespaces' refusals one row.
   Applied: absorbed at the refusal too, unless the query layer answers.
   Pinned (`ASelectRefusedPartWayThroughItsBindIsTheRowOfWhatItHadBound`).
2. **An `OFF` whose example binds nothing answered without the hold's
   bound**, so it could say `unchanged` before the drop that ended the
   opt-in was durable. Applied: it asks the one writer with no binding,
   which writes nothing and reads the bound under the hold.
3. **An `ON` racing a `DROP TABLE`** released its borrow before its write:
   the drop could commit and end the relation's opt-ins in the gap, and the
   row written after stayed opted over a dropped oid until a mount.
   Applied: the borrow is held across the write.
4. `sys.patterns`' new columns in `sys-views.md` §5 and the manual; the
   binding header's "`Rewind`, in `ResolveNames`" now names both paths.
5. One fill of the opted map for both lookups
   (`EnsureOptedPatternsFilled`), and `StatementBinding::Holds` for the
   two loops that asked whether a row names a relation.

The three cells it named are added: a drop inside `BEGIN ... COMMIT` and
`ROLLBACK`, over a join whose second relation is the one dropped; the
refused partial bind.

**Cells.** `tests/waystone_opt_in_test.cpp` (an opt-in is its example's
binding - the other namespace's text neither records nor replays, and
both opted keep their trails; `OFF` by example; a committed drop, on a
whole instance, and the name created again; a drop in a transaction; the
mount ending a dropped relation's opt-in), `tests/catalog_row_test.cpp`
(the 178-byte layout), `tests/pattern_rows.hpp` (a test's row by fetch id,
by scanning) and the calls of the opt-in API in twelve test files.

**The suite**: 1 failed of 4899 at `-j8`, run beside another session's suite - `StrictParkRigTest.StrictCommitsStagedDuringOneSyncShareTheNext`, the timing cell `known-gaps.md` (Testing) names - which passed run again alone; 0 failed of 4897 on the same tree before the review's fixes (2 disabled, 2 skipped), the hook's range at `-j8`, on the second
commit's build inputs. **`scripts/sim.sh`**: 280 runs, 0 failures, on the tree before the review's fixes, which change no logged write; not run again after them. Overhead not
measured; measured at the milestone's close.

### DI-S4 - a plan that matches nothing, 2026-11-20

On worktree `di-shape-identity`, over `6bedc13f`. DI-Q5 (a) is the
operator's; **DI-Q10 adopted (a) as DI-S0 restated it, and DI-Q14 and
DI-Q15 - written into §4 by this stage, which reached them - adopted (a),
on the standing go-ahead.**

**Built.** `exec::PlanRowCanMatch` (`exec/plan_catalog.hpp`): false only
where the row itself establishes that no statement can produce the plan's
key (DI-R9) - a relation the stored statement names is no longer named
that, read as the name each reference of its top-level block spells
against the current name, and namespace where written, of the oid recorded
at that position; or a column the statement writes by one of its bindings
is no column of that binding's relation. No resolution, so no namespace,
which the row does not store. `sys.plans` reads such a row `invalid`,
**for a captured order as for an administrator's plan** - CX-Q16's
`INVALID`, for the case that can be established - and `DROP TABLE`'s
RESTRICT passes it by. Nothing is written at the rename and nothing
removed: the state is derived at the read and at the drop, and a rename
back puts the plan in force again.

**Not built, and said** (`known-gaps.md`, "Captured plans (CX)"): a renamed
column the statement writes unqualified; a captured order that stops
compiling for any other reason; a statement stored cut short. Each reads
as it did.

**The review** (`critics-developer`, of the working diff over `6bedc13f`).
Four findings, a race, two document findings and three simplifications.

1. **An undecided `DROP COLUMN` read as a rename** `[quiet-wrong]` in this
   stage's sense: the column check asked for a visible column, a dropped
   one is not, and the schema read is latest state. A drop of another
   relation of the plan was then let through, and a rollback of the
   `DROP COLUMN` left the plan in force over a relation that was gone.
   Applied: a dropped column counts as found - a rename, decided when it
   is made (`alter.md` AL6), is all this check establishes.
2. **A stored statement cut inside a name could still parse - as another
   name** - and read `invalid` with nothing renamed. Applied: the parse is
   trusted only where it fingerprints to the key's own fetch id, which a
   whole text does (a literal, a `?` and the `0` read in its place are one
   marker) and a cut one does not. Pinned by the cell that keeps a plan
   over a string literal, a number, a written namespace and aliases
   `enabled`.
3. **A rename back after the drop's check orphans the plan.** Not fixed:
   closing it means the drop taking a lock on every other relation of
   every plan it passed by. Entered in `known-gaps.md`.
4. The RESTRICT read every row of a name where the newest is the plan.
   Applied.
5. The manual said "in force again" where a disabled plan comes back
   disabled, and "`table.column`" where the check is by binding; a `LIKE`
   is never read (its pattern must be a string, and a literal is read as
   `0`), and `invalid` hides `enabled` and `disabled`. Corrected, and in
   `known-gaps.md`.

Simplifications: the table list is read once per view read and per drop,
and the oid loop is a `find_if`. **Rejected**: the review's exact rule for
telling a namespace from a binding by the reference's byte offset - which
token that offset names for `ns.table` was not read, and the rule in place
errs only toward not checking. Its answers on what else might need the
test stand as it gave them: `ALTER PLAN` on such a plan is allowed, a
mount installs it (a rename back needs it there), and the pinned index's
and Cabin's RESTRICT still hold, entered as a gap.

**Cells.** `tests/admin_plans_test.cpp`: a rename of a column, then of
the relation, each read `invalid` and read `enabled` again once renamed
back, RESTRICT with it; an unqualified column's rename not established; a
captured order's row.

**The suite**: 0 failed of 4903 (2 disabled, 2 skipped), the hook's range at `-j8`, on this commit's
build inputs. Overhead not measured; measured at the milestone's close.
