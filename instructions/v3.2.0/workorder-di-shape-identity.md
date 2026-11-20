# Work order DI - a shape's statistics named by what the statement binds: `sys.statement_stats` keyed by the relations a statement resolved to, and a plan that matches nothing said so, the census before anything is built

Status: **WRITTEN 2026-11-20, not opened.** Written in the main checkout on
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

No stage is started. Every `[source-read]` claim below was read at
`f4138860`. No suite was run: the order changes no build input.

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
| DI-Q0 | The order's standing. | **a census first**: DI-S0 builds nothing and may narrow DI-S1 and DI-S2, or cut one | (b) a build order from §2 as written |
| DI-Q1 **marked (a)** | Where the binding enters. | **I-a**: the statistics key; the fetch id does not move | (b) I-b |
| DI-Q2 **marked (a)** | A statement with no binding. | **one row per `(fetch_id, kind)`, binding empty, the view says so** | (b) not recorded |
| DI-Q3 `[quiet-wrong]` | What the binding is. Spoken as "the plan key"; restated because the plan key exists for searched joins only (§0). | **B1**, capped at `kMaxPlannedRelations`; what a statement binding more does is DI-S0's finding, brought back before DI-S1 | (b) B2; (c) B3, with a collision's merge accepted and stated; (d) as spoken: the plan key where one exists, today's row elsewhere |
| DI-Q4 **re-marked (b)** | The Waystone opt-in's reach across namespaces and a drop. | not in DI: no result is wrong, and it is a `sys.patterns` format decision - marked first, then overruled by "every path" | **(b) in DI**, the operator's |
| DI-Q12 | The Cabin controller's signal (§0a). | **keyed by the pattern and its candidate together**, so one text over two relations is two signals and each relation's Cabin is priced by its own heat; `kMaxTrackedFingerprints` then bounds signals, not patterns | (b) keyed by DI-Q3's binding, as the statistics are; (c) a pattern whose candidate moved between two relations is not credited to either |
| DI-Q13 `[quiet-wrong]` | What an opt-in names once it is in DI. | **the shape under the relations `WAYSTONE ON`'s example resolved to** - it already compiles its example under the session's namespace (`waystone-concpets.md` §4a) - so a drop ends it and another namespace's same text is not opted. A `sys.patterns` row change, so a format event (DI-Q11); `WAYSTONE OFF <select>` after a drop, which today needs no relation, is restated in DI-S0 | (b) the opt-in stays by text and the manual says so, the recording cost accepted - what "every path" rules out unless the operator re-marks |
| DI-Q5 **marked (a)** | A renamed relation's plan reading `enabled`. | **in DI** | (b) left a known gap |
| DI-Q6 | The views' shape. The grain of a row changes from a shape to a shape under one binding. | **`sys.statement_stats` and `sys.statement_histogram` gain `relations` (oids as bound) and `namespace`; one row per binding; no second view** | (b) the old grain kept and a second view added per binding |
| DI-Q7 | Rows of a relation that is gone. | **kept and readable until the part is full, then the first reclaimed**; counted in `sys.meta` | (b) removed at the drop's commit on every core - a cross-core act DI-R4 does not allow on the statement path and DI-S0 would have to place; (c) kept until restart, as every row is today |
| DI-Q8 | The bar at the close. | **DA's close cells and a point `SELECT` and keyed `INSERT` at `cores` 1 and 8: no cell slower outside its own spread**; no factor proposed, since DF's 0.5 us bar on this path was not resolvable | (b) a bar the operator names |
| DI-Q9 | The session's published statement and ASH. | **left on the fetch id** unless DI-S0 finds a reader that joins a sample to a statistics row; then brought back | (b) the binding published beside the fingerprint - a word or more written at every statement's head |
| DI-Q10 | Which plans-that-match-nothing DI shows, beyond the rename. | **the rename and a captured order that no longer compiles (CX-Q16's `INVALID`), by P1**; a fingerprint-version row stays the mount's | (b) the rename alone; (c) P2 |
| DI-Q11 | A format event. | **one, for DI-Q13 alone**: the statistics and the controller's signals are memory-resident and P1 persists nothing. The bump is the host DA-S3 waits for | (b) P2 admitted too |

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
