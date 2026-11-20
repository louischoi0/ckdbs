# The `sys` views, and `SHOW` as sugar over `SELECT`

**Owns** SV1-SV5, the registry of `sys` views, each view's filter class and
key, the four memory-resident statistics views and `sys.meta`. Built by BO
(`instructions/v3.0.0/workorder-bo-sys-statistics-show-sugar.md`, the work
order that holds the reasoning and the stage logs). The registry is
`include/kds/server/sys_views.hpp`; every builder is a `CommandDispatcher`
member in `src/server/sys_views.cpp`; the `SHOW` rewrite is
`CommandDispatcher::HandleShow`'s table in `src/server/command_dispatcher.cpp`.

## 1. The rules

- **SV1 - one `SELECT`, one view.** Every `SHOW <target> [<argument>]`, and
  `DESCRIBE`/`DESC <t>`, names exactly one view `sys.<v>` and is rewritten
  by the dispatcher into one `SELECT` over it - its AST, built from the
  line with no parse, not its text, so a refusal
  on the argument names the byte the client wrote: a fixed column list (or
  `*`) and, where the target takes an argument, a fixed `WHERE` on the
  view's key. It runs through `HandleCatalogView`, the function a `SELECT
  ... FROM sys.<v>` reaches, and no other.
- **SV2 - the same result.** The `SHOW` spelling and the `SELECT` it stands
  for answer the same rows, the same columns in the same order and the same
  values, and refuse with the same code at the same byte. The `show_sugar`
  contract suite (`tests/show_sugar_contract_test.cpp`) pins it through
  both protocols.
- **SV3 - nothing only `SHOW` can read.** A value a `SHOW` reports is a
  column of a view. A counter with no row of its own is a `sys.meta` field.
- **SV4 - a view is the only place the data logic lives.** A `SHOW` target
  is a view plus one rewrite entry; no `SHOW` handler reads a source.
- **SV5 - a view answers as an ordinary table** (the operator's W4: *"Every
  sys-related table must be output in the form of an ordinary table."*). A
  view's result is described and emitted through the statement's
  `ResultSink` as a relation's `SELECT` is: on KWP a typed `S_ROW_DESC` and
  typed row batches, never one `TEXT` column; on the newline protocol the
  header line and rows a relation's `SELECT` prints. **`ANALYZE` is the one
  text-lines reply left**: a plan, not a table.

## 2. What a view admits

A view is materialized by its builder and then filtered, so it admits less
than a relation:

- **Admitted:** a projection, `*`, a qualified column (`FROM sys.tables AS
  t ... WHERE t.oid = 100`), `WHERE` conjuncts `column op literal` and
  `column BETWEEN literal AND literal`,
  `ORDER BY` under each column's declared type, `LIMIT` and `OFFSET`, and a
  view in a `SET` value's subquery.
- **Typed.** A column is `uint64`, `varchar` or `timestamp` (UTC
  microseconds, `types.md` TY4). A `WHERE` literal is coerced to the
  column's type before any row is read (`types.md` §3.1a): `name = 5`,
  `oid = 'x'`, a negative integer and one past 2^64 - 1 are refused with the
  literal's byte. Integers compare unsigned; `sys.patterns.fetch_id` uses
  the full 64-bit range.
- **NULL** is a value a row does not have - an unreadable index's height, a
  Cabin's counters where no store runs, a relation's name for an oid the
  catalog no longer lists - never a 0 that reads as one.
- **Refused:** aggregation (AG12: a view's rows come from no step chain), a
  join, a subquery predicate, a boolean conjunct (`OR`, `NOT`, an `IN`
  list, `LIKE`, `NotImplemented`), a function call (`NotImplemented`), a
  column-to-column comparison, and `ANALYZE` over a view. A panel that needs
  counts reads the rows and counts them (`tools/ash_panel.py`). A form only the
  query layer reads (`parser-v2.md` I19: a value expression, an alias,
  `DISTINCT`, a set operation) is refused `NotImplemented` at the view's
  byte. The aggregation, join, `ANALYZE`, subquery-predicate and
  column-to-column refusals carry no byte (`known-gaps.md`).

## 3. Filter class (BO-Q2) `[quiet-wrong]`

A view that reports **schema objects** answers under the session's read
view, as `ddl-transactional.md` §5a requires: `tables`, `objects`,
`columns`, `namespaces`, `indexes`, `fkeys` (a foreign key is listed where
its child relation is visible). Every other view reports **engine state**
and reads no session view, so it neither filters nor ends a statement
boundary under an open DDL. A view on the wrong side of the line either
leaks an uncommitted `CREATE TABLE` or hides engine state a diagnostic
needs, with no error.

## 4. A view's key (BO-Q12)

A view may declare one key column (`sys_views.hpp`):

- **`kRequired`** - `sys.page_slots` by `page_id`: it cannot be
  materialized whole (it would read every page), so a `SELECT` without
  `page_id = <literal>` is refused `InvalidArgument` naming the key.
- **`kChecked`** - `sys.columns` by `rel_name`: optional; when written, a
  name that resolves to nothing is refused, never answered with no rows
  (`[quiet-wrong]` otherwise: a refusal turned into an empty answer). A
  bare name takes its namespace from a `namespace = '<literal>'` conjunct
  when there is one, else from the session (NS5).

The builder takes the first key conjunct as a bound on what it reads and
returns only what it names, so that conjunct is not compared again.

## 5. The registry

| view | filter | key | what a row is |
|---|---|---|---|
| `tables` | schema | - | a relation: oid, namespace, name, descriptor and root page ids, `clustered_type` (`HEAP`, `BTREE`, `CHAIN`), `next_id`, spare slots and how many are free |
| `columns` | schema | `rel_name`, checked | a live, visible column of a relation (no dropped column, no heap's hidden key) |
| `objects` | schema | - | a table object of `sys.objects` |
| `types` | engine | - | a registered type |
| `patterns` | engine | - | a `sys.patterns` row - a shape ever opted in to Waystone (`waystone-concpets.md` §4): `oid`, `fetch_id` (decimal), `fingerprint_version`, `stmt_class`, `waystone`, `use_count`, `last_seen`, `waystone_root`, `dir_depth`, and what the row's `WAYSTONE ON` bound (DI-S3) - `namespace`, `relation_oids`, `relations`, as §6.1 prints a statistics row's. `waystone` (1 or 0) replaced `origin` and `pinned` at CF-S1b, and is 1 only for a current-version row with the opt-in bit set, so a lapsed row reads 0 and its `fingerprint_version` says why; `use_count`/`last_seen` frozen at 0 since BL-S4; root and depth NULL where the row has no directory |
| `namespaces` | schema | - | a namespace, `sys` and `public` first |
| `indexes` | schema | - | a secondary index, its keys, covered columns, height and entries, how often it served a read (§6.2b) and how often its leaves split (§6.2c) |
| `fkeys` | schema | - | a declared foreign key, listed where its child is visible |
| `access_stats` | engine | - | an access shape `(rel, kind, columns)`, every core's part merged (§6.2d) |
| `relation_stats` | engine | - | a relation's Keystone budget, its tree's shape, the join planner's prior, its write activity (§6.2), its lock waits (§6.2a) and its space (§6.2c) |
| `cabins` | engine | - | a Cabin and its store counters |
| `assertions` | engine | - | an assertion, whether it is enforcing, and its counters |
| `transitions` | engine | - | a declared Transition, verbatim |
| `statistics` | engine | - | the planner's relation statistics (CY-S1b, CY-S4, `plan-stats.md` PS23): `relation_oid`, `namespace`, `name`, `kind` (`relation`), `state` (`published`, `pending`, or `none` for a locked relation that holds nothing), `origin` (`gathered`, `imported`, or `observed` for the steward's sampled prior, whose `version`, `leaves` and `made_at` are NULL), `version`, `rows`, `leaves`, `made_at`, `locked`, `used` (1 on the row the planner prices the relation from), `document` (the row's export text, `EXPORT STATISTICS`'s answer) |
| `statistics_history` | engine | - | the replaced managed versions, oldest first, the last 8 per relation (CY-Q12 (a)): `relation_oid`, `namespace`, `name`, `version`, `origin`, `rows`, `leaves`, `made_at` |
| `plans` | engine | - | a managed plan's `sys.plans` row (`plan_id`, `name`, `origin`, `state`, `fetch_id`, `relations` as written, `join_order` as run, `variants`, `written_at`, `statement` with its literals written `?`). `origin` is `captured` (a captured join order, CX-S1a; `state` `enabled`, `variants` NULL) or `admin` (an administrator's plan, CX-S1b): its `state` is `enabled`, `disabled`, or `invalid` where a relation it names is gone or its fetch id is another fingerprint version's. **Either origin reads `invalid` where a rename has put its key out of every statement's reach** (DI-S4, `exec::PlanRowCanMatch`): a relation its stored statement names is no longer named that, or a column the statement writes by its binding is no longer one - derived at the read from the row and the catalog, nothing written, and only what is established (an unqualified column's rename is not). An administrator's plan has its `join_order` its `ELSE` variant's, and `variants` every variant in rule order as `<name> WHEN <rule> ORDER (<binding>, ...)` or `<name> ELSE ORDER (...)`, each followed by ` ACCESS <binding> <pin>, ...` where the variant pins an arm (CX-S3a), joined by `; `. A row this build cannot read shows `unreadable` and its text as stored |
| `plan_advice` | engine | - | an `ADVISE PLAN`'s latest advice for one statement shape (CX-S7, `parser-v2.md` I12): `fetch_id`, `relations` as written, `in_force` (the plan it was priced against: `written`, `captured`, `uncaptured`, `searched`, or `admin:<plan>/<variant>`), `searched` (0 for a join the search never orders), `proposed_order`, `cost_ratio` (the proposal's cost over the plan in force's, as text with three decimals; under 1 cheaper), `generation` (the statistics generation the search read), `advised_at`, and `proposal`, the `CREATE PLAN` text over the statement's shape with its literals `?` (NULL for a block `CREATE PLAN` refuses, a subquery predicate's). Memory only, one table for the instance, emptied by a restart, oldest first; bounded at 1,024 shapes `[provisional]`, the oldest dropped past it; a relation dropped since reads `oid <n>`. Never read by a compile |
| `cabin_optimizer` | engine | - | a managed entry of the Cabin controller; refused where no controller runs |
| `lock_waits` | engine | - | one lock wait (§6.3) |
| `statement_stats` | engine | - | one statement shape (§6.1) |
| `statement_histogram` | engine | - | one non-empty log2 latency bucket of one statement shape (§6.1a) |
| `refusal_stats` | engine | - | one error code a core has refused a statement with (§6.1b) |
| `checkpoints` | engine | - | one checkpoint run (§6.4) |
| `wait_event_names` | engine | - | one wait event of the registry (§6.5) |
| `wait_stats` | engine | - | one event on one core, plus a NULL-core total row per event (§6.6) |
| `wait_histogram` | engine | - | one non-empty log2 bucket of one event on one core (§6.6) |
| `txn_histogram` | engine | - | one non-empty log2 bucket of the lives of one core's explicit transactions, by access mode (§6.1c) |
| `session_waits` | engine | - | one event a session has waited on, cumulative (§6.6) |
| `session_state` | engine | - | one session's statement task as last published (§6.6) |
| `ash` | engine | - | one active-session sample (§6.7) |
| `page_slots` | engine | `page_id`, required | one page's header row, then one row per slot - or, on an internal node, its leftmost child, then one row per entry |
| `meta` | engine | - | one `sys.meta` field: `(section, field, value)`, the session's core's (§7) |

| spelling | answers with |
|---|---|
| `SHOW META` | `SELECT * FROM sys.meta` |
| `SHOW TABLES`, `NAMESPACES`, `PATTERNS`, `ACCESS`, `CABINS`, `INDEXES`, `FKEYS`, `ASSERTIONS`, `TRANSITIONS`, `PLANS`, `CABIN_OPTIMIZER` | `SELECT * FROM sys.tables`, `sys.namespaces`, `sys.patterns`, `sys.access_stats`, `sys.cabins`, `sys.indexes`, `sys.fkeys`, `sys.assertions`, `sys.transitions`, `sys.plans`, `sys.cabin_optimizer` |
| `SHOW BUDGET` | `SELECT oid, namespace, name, next_id, ids_issued, ids_remaining, budget_used_ppm, budget_warning, budget_exhausted FROM sys.relation_stats` |
| `SHOW PAGE <id>` | the header and slot columns of `sys.page_slots WHERE page_id = <id>`; `SHOW PAGE <id> VALUES` adds `value`, the payload in hex |
| `DESCRIBE <t>`, `DESC <t>` | `SELECT * FROM sys.columns WHERE rel_name = <t>` |

`lock_waits`, `statement_stats`, `statement_histogram`, `refusal_stats`,
`txn_histogram`, `checkpoints` and `relation_stats`'
activity have no `SHOW` spelling (BO-Q10), and neither do `types`,
`objects` or an unkeyed `columns`. `SHOW TRACES`, `SHOW TRACE` and
`TRACE ON` are retired with the trace sink nothing ever installed (BO-Q9);
an unknown target is refused at its byte.

## 6. The statistics views

**Memory-resident** (BO-Q5 (b)): no oid, no root page, no format change;
emptied by a restart and by a restore. Each keeps **one part per core**
(`stats::kStatsParts`, asserted equal to `kMaxCores`): the core a row
describes is its part's only writer. **A write never fails, delays or
reorders what it describes, and never waits** (`docs/rules/rules.md` §3a;
DL-S2) - in one of two shapes:

- **a row of fixed counters takes no latch**: relaxed single-writer words
  in a part that is never moved, which a builder on any core reads as they
  stand - `sys.relation_stats`' activity and lock columns, the leaf
  counts, `sys.indexes`' usage, `sys.access_stats`, the Cabin store's
  counts (`stats/counter_table.hpp`, `stats/access_stats.hpp`), as
  `sys.refusal_stats` and the transaction counts always were. A row's
  columns may differ by an event in flight. **Such a part is a fixed table
  allocated whole at its core's first row** - about 576 KiB a core for the
  relation counts, 384 KiB each for the leaf counts and the index usage,
  192 KiB for the Cabin probe counts, where the maps they replaced grew
  with use - and never grown: its bound is the store's own (§6.2, §6.2b);
- **a row that is more than counters is under its part's latch**
  (`LatchKind::kStatsRing`), which a record takes by the try form:
  `sys.statement_stats`, `sys.lock_waits`, `sys.checkpoints`, `sys.ash`,
  the planner's parts. The one other taker is a builder's copy - room made
  before the hold, nothing merged or decoded under it - and a record that
  meets it is dropped and counted, apart from the bound's drops:
  `statement_stats_busy`, `lock_waits_busy`, `checkpoints_busy`,
  `ash_busy`, `plan_stats_part_busy` (`sys.meta`).

A row past a bound or an allocation that fails is dropped and counted in
`sys.meta` (SV3). No row is read by the engine, so none can change a
result - a lost or wrong row costs a reader's insight, never an answer. **No switch**
(BO-Q8): recording is always on - but for `sys.access_stats` (§6.2d),
which kept the `access_statistics` key it had as a catalog relation
(DA-Q4 (a)). At BO's close it cost 0.35-0.52 us of
server CPU per point statement, inside the measurement's spread
(`bench/v3.3.0/results-bo-close-v3.3.0-23-g86d1fe1a.md`); that number is of
the statement's record before DF widened it. What DF added costs a tuple
key read about +0.5 us more at `cores = 1` - §6.1's cost columns +0.44 us
(`bench/v3.4.13/results-cx-close-bisect-v3.4.13-1-g11f72ce9.md`), §6.1c,
§6.2b, §6.6's two ages and §7's `txn` fields +0.07 us together - and an
index-served read +0.18 us beyond §6.1's share, which was not measured on
that shape; a write is inside the measurement's floor
(`bench/v3.4.13/results-df-close-final-v3.4.13-17-gc16b4fc2.md`). `sys.refusal_stats` (§6.1b) and the transaction statistics
(§6.1c, §7's `txn` counts) are the two stores with no latch: each a fixed
table of single-writer words a core.

### 6.1 `sys.statement_stats` (`stats/statement_stats.hpp`)

One row per `(fetch_id, kind, binding)` for the instance, the cores' parts
merged at read: `binding`, `namespace`, `relations`, `relation_oids` (below),
`calls`, `refused`, `total_us`, `max_us`, `first_seen`, `last_seen`,
`plan_source`, `plan_name`, `variant`, the cost columns below
and `shape_text`, the first statement seen, cut to 256 bytes at a character
boundary. **`plan_source`** (CX-R11) is where the shape's join order came
from at its latest compile, on the core that saw the shape last: `admin`
(an administrator's plan, named by `plan_name` and `variant`, which are
NULL otherwise), `captured`, `searched`, `deferred` (searched, not
captured), `unkeyed`, `over_ceiling`, `declined` (a captured order declined
for the statement), `admin_declined` (a plan's variant declined; `plan_name`
and `variant` name it) or `written`. **NULL is a block the join planner
did not order and no plan chose**: a statement over one relation under
every `join_order` - unless it has an administrator's plan (CX-S3b), which
reads `admin` or `admin_declined` - and under `planned` and `fixed` also a join of tuple
key descents alone, a `SELECT *` join and a join with a subquery
predicate, each of which runs its written order unsearched (under
`written` those three read `written`). A subquery's compile never sets
it: the row is the statement's.
It is how a DBA or an agent finds a shape that runs uncaptured. `kind` is the first whitespace-separated token, upper-cased, when
it is a verb the dispatcher routes, else `OTHER`; only `SELECT`, `INSERT`, `UPDATE`
and `DELETE` carry a fingerprint, so every other kind is one row at `fetch_id = 0` -
but `ANALYZE <statement>`, which is kind `ANALYZE` under the shape of the
statement it names, a refused `ANALYZE DELETE` included.
Recorded once per statement at its end, timed whole - a park, a re-run and
the group commit are one call; a refused statement is a call. A statement
the engine issues itself (a listener's `ROLLBACK`, a KWP load's
`BEGIN`/`COMMIT`) and a load chunk are not counted. 4096 shapes per core;
`statement_stats_dropped`. **A shape's slot is 768 bytes** beside its
168-byte key and its text - about 1.2 KB a shape with a 256-byte text - so
a core's part holds at most about 5 MB and 64 cores' about 310 MB, each
allocated on a shape's first statement and kept until the instance stops.

**A row is one binding's** (`stats/statement_binding.hpp`, DI-R2). A fetch
id is a hash of the statement's text and holds no namespace and no oid, so
one text is two statements wherever a name in it resolves to two relations:
under two namespaces (`USE`, NS5), or over a relation dropped and created
again under its name (DT2). The row's key therefore carries what the
statement bound: **the distinct relations it declared at its binds, in
first-declared order** - a join's, a subquery's, a write's target and then
its subqueries', a query layer statement's as its compile describes them -
**and the namespace its unqualified names were resolved in**. It is the
resolution that answered: a resolve made again because a DDL moved under
it forgets the first, and a query layer walk, which runs or not by the
data, declares nothing into it. Two statements that resolved any
reference differently are never one row: under one default namespace two
written names are one relation only where they are one name, so equal
lists are equal resolutions of every reference, and the namespace is what
tells `a.t JOIN b.t JOIN t` under `USE a` from the same text under
`USE b`. A statement that writes every name qualified has no namespace and
is one row whatever namespace its session is in.

- `binding`: `bound`; `none`, a statement that declared no relation -
  refused before its bind, naming none, or of a kind with no fingerprint;
  or `over`, a statement that declared more than 16 distinct relations,
  whose binding is not held: every such statement of a shape is that one
  row, and the row is no binding's.
- `namespace`: the name of the namespace the unqualified names were
  resolved in, `oid <n>` once it is gone; NULL where the statement wrote
  none, and for `none` and `over`.
- `relations`: the bound relations, each `ns.name`, or `oid <n>` once it
  is gone, joined by `, ` as `sys.plans`' are; NULL for `none` and `over`.
- `relation_oids`: the same relations by oid, joined the same way. **The
  identity a reader groups and joins by**: a name moves with a rename, and
  a relation dropped and created again reads the same name on two rows.

A statement refused part way through its bind is the row of what it had
declared by then - a `SELECT` refused in its compile, a write refused past
its target - unless the query layer answers in a native compile's place,
whose binds are then no part of it. **A `SELECT` the native grammar does not parse has no
fetch id** - a set operation, and every other form only the query layer
reads - so it is kind `SELECT` at `fetch_id = 0`, one row a binding: the
row of every such text over those relations. `plan_source`, `plan_name` and `variant` are the row's
own binding's. The relations are not the references of a captured order's
key (`sys.plans`' `relations`, a searched block's references with their
repeats): for a planned block with no self-join the two are the same
relations in the same order.

**A relation that is gone keeps its rows until the instance stops.** Its
oid is never reissued, so its rows take no further call; they stay
readable and count against the part's bound. Nothing reclaims them: a
record does nothing but record (`rules.md` §3a), and a new shape that
meets its core's part full is dropped and counted.

**What a shape costs**, between `variant` and `shape_text`: `conflicts`,
`rows`, `rows_examined`, `pages`, `compile_us`, `timed_calls`, `cpu_us`,
`sched_us`, `commit_us`, `io_us`, `net_us`, `app_us`, `other_us`,
`page_reads`, `log_bytes`, `log_records`. **A column the engine could not
fill for any call of the shape is NULL, never 0, and a call it could not
fill for is in no sum.** The time split, the page reads and the log are
the statement's whole, its re-runs and waits included; `rows`, the chain's
counts and `compile_us` are its last run's. Every one of them is summed
over the shape's calls - `compile_us` over the calls that compiled one, so
`compile_us / calls` is a mean per call (per compile only where every call compiled), never one compile. A refused call adds no `rows`
and no chain count; it keeps its time, its log and, where it got that far,
its compile.

- `conflicts`: the calls refused `TxnConflict`, a subset of `refused`, by
  the code the client was told - so a deadlock's victim is one.
- `rows`: what a statement gave its result sink - produced, not what a KWP
  `max_rows` let leave - or reported affected; a `RETURNING` write's rows
  are counted once, as affected.
- `rows_examined`, `pages`: the rows and pages the statement's own step
  chain counted (`pages` exact on a walk and one a keyed access, whatever
  the tree's height), summed over the calls whose chain ran to its end: a
  native `SELECT`'s, a fold's, an `ANALYZE`'s. **NULL where no chain of
  the statement's own counts**: an `INSERT`; an `UPDATE` and a `DELETE`,
  whose walk is the dispatcher's; a query-layer `SELECT`; a view's
  `SELECT`. A subquery's chain and a query-layer walk's are in no row.
- `compile_us`: from the head of the call's last run to its own chain
  compiled - a `SELECT`'s chain, an `UPDATE`'s or `DELETE`'s `WHERE` - so
  the parse and the name resolution are inside it, and for an `UPDATE` so
  are its `SET` subqueries, which run before its `WHERE` compiles. NULL for
  a shape that compiles none.
- `timed_calls`: the calls that ran inside a task of their own session,
  which is every statement a listener submits and no statement a
  synchronous dispatch runs outside a task. **Only those have a time split,
  page reads and a log count**, and each of the columns below is NULL where
  `timed_calls` is 0.
- `cpu_us` and the six after it: the session's wait statistics at the
  statement's end less at its head, what `ANALYZE`'s DB-time line reads
  (§6.6). `cpu_us` is ON_CPU time; `sched_us`, `commit_us`, `io_us`,
  `net_us` and `app_us` are the wait classes Scheduler, Commit, User I/O,
  Network and Application (`wait-events.md`); `other_us` is every other
  class of DB time, so the seven sum to the timed calls' DB time. The
  stall a blocking read puts on the core's other tasks is no statement's
  (CJ-Q8).
- `page_reads`: the statement's own blocking reads, the counts of
  `io.page_read`, `io.var_heap_read` and `io.scan_ring_read` in that
  delta. There is no hit count: `pages` is a logical count.
- `log_bytes`, `log_records`: the log records the statement's task
  appended, by encoded size, booked at the append to the session of the
  task being polled (`sched::BookLogAppend`). They include what a statement
  appends under no transaction (an issued key's mark, a `PURGE`'s records)
  and an autocommit statement's commit record; an explicit transaction's
  commit record is its `COMMIT`'s. A record appended outside a statement
  of the session - a checkpoint's, the listener's `ROLLBACK` at a close or
  at the lifetime ceiling - is no shape's.

### 6.1a `sys.statement_histogram` (`stats/statement_stats.hpp`)

A shape's latency distribution: `fetch_id`, `kind`, `binding`, `namespace`,
`relation_oids` (the `sys.statement_stats` row's, the columns the two views
join by), `bucket`, `lo_us`, `count`. Every call's elapsed time - `total_us`'s, a refused call's
included - falls in one of `sys.wait_histogram`'s 32 buckets (§6.6): bucket
k holds `[2^k, 2^(k+1))` microseconds, the last is open-ended, and bucket 0
takes everything under 2 us. One row per shape and non-empty bucket, the
cores' parts merged as `sys.statement_stats`' are; a shape's counts sum to
its `calls`. It shares the shape's slot and bound.

### 6.1b `sys.refusal_stats` (`stats/refusal_stats.hpp`)

How statements were refused: `core`, `code`, `count`, `last_seen`. One row
per core and code with at least one refusal; `code` is the name of the
wire's pinned category the client was told (`protocol.md` §11:
`TXN_CONFLICT`, `ALREADY_EXISTS`, ...), and a refusal built from a bare
message reads `INVALID_ARGUMENT`, as its line reads back. A deadlock's
victim reads `TXN_CONFLICT`; `sys.lock_waits`' `outcome` tells the two
apart, in a ring that overwrites. Counted once per refused statement at
its end, where `sys.statement_stats`' call is, so a refusal the session
layer answers before the dispatcher - a protocol error - is in neither.
Bounded by the registry: 64 codes a core, a fixed 1 KB, no allocation, and
keyed by nothing a client can mint. Read without a latch, so a row's
`count` and `last_seen` may differ by a refusal in flight.

### 6.1c `sys.txn_histogram` (`stats/txn_stats.hpp`)

How long explicit transactions live: `core`, `access`, `bucket`, `lo_us`,
`count`. A life runs from the head of the transaction's `BEGIN` to the
instant its end is handled - its `COMMIT`, its `ROLLBACK`, the listener's
rollback at the lifetime ceiling or at a close - and falls in one of
`sys.wait_histogram`'s 32 buckets (§6.6). A `strict` commit that parks for
its sync is recorded before that park, so its wait is not in its life.
`access` is `read write` or `read only`: a `BEGIN ... READ ONLY`
transaction's life is kept apart. One row per core, access mode and
non-empty bucket. **An autocommit statement's transaction is not here**;
its statement is in `sys.statement_histogram`. The store is §7's `txn`
counts' (a fixed 0.6 KB a core, no latch, no allocation), and an instance
with no clock records no life.

### 6.2 `sys.relation_stats`' activity (`stats/relation_stats.hpp`)

`inserts`, `updates`, `deletes`, `purged`: the rows a relation's write
statements reported since the instance started, every core's summed. A
statement is counted when it answers, not when its transaction decides -
rows an explicit transaction rolls back, or an autocommit statement whose
deferred commit a fail-stopped log aborts, stay counted. A KWP load chunk's
rows are inserts; a `PURGE`'s retired keys are counted whatever its decide
does (PU6). The budget columns are exact, through `BudgetOf`; `height` and
`leaves` walk the clustered tree; `est_rows` and `generation` are the join
planner's published prior (`plan-stats.md` §6). **The bound, 4096 relations
per core, counts every relation a core has written since the instance
started, dropped ones included** (BO-Q15 (b)): past it, that core's writes
of a relation new to it are dropped, the view under-counts the relation,
and `relation_stats_dropped` above 0 says so.

### 6.2a `sys.relation_stats`' lock waits (`stats/relation_stats.hpp`)

`lock_waits`, `lock_wait_us`, `lock_deadlocks`, `lock_fault_nets`: the lock
waits registered on a unit of the relation since the instance started,
every core's summed - how many, their time, and how many of them ended
`deadlock` and `fault_net`. **Counted where a `sys.lock_waits` row is
recorded** (§6.3), from that row's relation, time and outcome, so the four
keep what that ring has overwritten - and a row the ring lost to a
reader's copy (`lock_waits_busy`), since these columns take no latch - and
are bounded by it in nothing else: what records no row there is in none of
them. `lock_deadlocks` summed is
therefore the deadlocks that wrote a row, not every deadlock refusal - a
victim whose slot had already flipped writes none, nor does an online index
build's drain - and a wait a destroyed frame ended (`cancelled`) is in
`lock_waits` and `lock_wait_us` alone. They share §6.2's store, part and
bound: a relation's first wait on a core is that core's first sight of it.

### 6.2b `sys.indexes`' usage (`stats/index_stats.hpp`)

`probes`, `ranges`, `entries_scanned`, `rows_resolved`, `last_used`: how
often each secondary index served a read since the instance started, every
core's summed. Folded from the executor's per-step counters where a chain's
run is recorded: for every step an index served, the times the step walked
it between one pair of bounds - `probes` for an equality step, `ranges` for
a range step - the entries it read there and the tuple keys it then
descended for, and the instant of the latest such run: `last_used` is the
head of that run's statement - kept on the monotonic clock, so the fold
reads no clock, and shown as the wall instant that long before the read -
and is NULL for an index no read has used and on an instance with no
clock. **A walk is counted where the index is
read**: an index step under another step counts once per row of the outer,
a value list once per value (`index.md` IX8b), and a step that names an
index and walks its relation instead - the `indexes` key off, the index
dropped under a compiled chain, a correlated probe that could not key -
counts nothing.

**A run is folded when it completes.** A stopped run (a `LIMIT`) folds the
prefix it ran; an `ANALYZE` runs its statement, and that run is a use like
any other; a statement refused before or during its run folds nothing of
it. Each relation walk of a query-layer statement is a run of its own, so
a layer statement refused after a walk finished has folded that walk.

**What is not a step is not counted.** A unique index's check descents and
a foreign key's are not a step: `unique` is what says such an index is in
use. A write's maintenance of an index is no use, and the dispatcher's walk
of an `UPDATE`'s or a `DELETE`'s `WHERE` reads none; a write's own read
through the query layer - `INSERT ... SELECT`'s source - is a run and is
counted. The chain a write's `WHERE` subquery runs can be served by an
index, and its counters are dropped where it runs, so that use is counted
nowhere. `REINDEX` builds under a new oid, so a rebuilt index starts at 0.

**The instance owns the store**, unlike §6's others: a restart, a restore
and a second instance in one process each start empty with no reset. One
part per core (`stats/oid_parts.hpp`, §6.2's shape), the core a statement
ran on its only writer. **No latch** (§6): a core's first sight of an
index claims its row - a part's first sight of any index is the one
allocation; the counts are single-writer relaxed words that are never
moved or released, so the core keeps the pointer to an index's counts (its
dispatcher's eight-slot cache, by oid) and a repeated read adds through it
with no lookup and no allocation. A reader reads the words as they stand,
so a row's columns may differ by a run in flight. Bounded at
4096 indexes a core, dropped ones included; past it a new index's counts
are dropped and `index_stats_dropped` (`sys.meta`) says so. **Nothing reads
a count**: no advisor, no automatic drop (DF-R3).

### 6.2c Space (`stats/plan_stats.hpp`, `stats/leaf_stats.hpp`)

**`est_delete_marked`** on `sys.relation_stats`: an estimate of the
relation's delete-marked rows - rows a `DELETE` marked that no `PURGE` has
retired, which still take their room. It is the join planner's steward's
sample (`plan-stats.md` §4): the delete-marked slots of the leaves it read
for the relation's prior, scaled as it scales their live slots, exact for a
tree of one leaf. **As old as the relation's last seed that read a leaf**:
the steward comes back to a relation no sooner than 10 s after its last
seed, and later the more relations it keeps - it spends 16 page reads a
1 s step, a relation of height 2 costing about six - and **NULL for a
relation it has not sampled a leaf of**: it seeds a relation a searched
join block named, or one whose prior a clean shutdown carried, and no
other. Undecided
versions count as they lie, as PS1's do. **It is not a planner fact**
(DF-R8): it is kept beside the prior in a table this view alone reads, it
is in no snapshot, and it moves no `generation`.

**`leaf_divisions` and `leaf_compactions`** on `sys.relation_stats`, and
**`leaf_divisions`** on `sys.indexes`: how often the tree's leaves divided,
and how often a clustered leaf was compacted in place, since the process
started. Counted where the act is done, by the oid that owns the tree's
pages. **A division moves rows**: a full leaf whose new key sorts inside it
gives half its rows to a new leaf. **An append is not one**: a clustered
tree grown by a fresh leaf to the right of a full one, nothing moved - what
a rising key produces - is counted nowhere, so a relation loaded in key
order reads 0 however many leaves it has. An index leaf has no append
shape, so every split of one is a division, a build's and a rebuild's
included. A compaction is the rebuild in place of a full clustered leaf
over slots a rollback or a `PURGE` retired, taken in place of a division
(`heap-and-tuple.md` §5); an index leaf is never compacted, no index entry
being removed, and `sys.indexes` has no such column. A mount's replay
counts nothing. The store is the process's, as §6's are but §6.2b's -
the storage layer has no instance to ask - bounded at 8192 trees a core,
relations and indexes together; past it a new tree's counts are dropped and
`leaf_stats_dropped` (`sys.meta`) says so. A tree's first division on a
core allocates its count, under the divided leaf's hold; a failed
allocation drops the count, never the split.

### 6.2d `sys.access_stats` (`stats/access_stats.hpp`)

`rel_oid`, `namespace`, `rel_name`, `kind`, `columns`, `column_mask`,
`uses`, `last_seen`: one row per access shape `(kind, rel_oid,
column_mask)` for the instance, every core's part merged - `uses` summed
and saturating, `last_seen` the latest, on the instance's monotonic clock
in ns. What a shape is and what counts is `heap-and-tuple.md` §7's. **Rows
in key order** (DA-Q2 (a)): `rel_oid`, then the kind's name, then
`column_mask`. A catalog relation at page 11 until DA, whose dead
bootstrap left at superblock 38 (oid 130 retired, page 11 reserved and
unused); memory-resident since DA, like every view of this section, and
the instance's
(`Expeditor`'s, handed to every core; DA-R6), so a restart, a restore and
a second instance in one process each start empty.

**A part is a flat table of 40-byte slots, at most half of them live**
(DA-Q8 (a), DA-Q11 (a)), allocated whole at its core's first record and
never grown, **and takes no latch** (§6; DL-S2): a slot's state is one
atomic word, a record claims or adds, and a column change's forget marks a
slot dead where it stands and waits for nothing - the slot is free again,
for its own shape from zero or for a new one, and no tombstone counts
against the bound. A reader leaves out a slot that changed hands under its
copy; its bytes are the boot key `access_stats_bytes_per_core`
(DA-Q12 (a): `[80, 256 MiB]`, default 327,680, 4,096 shapes a core). A new
shape past a part's bound, or every record of a part whose table could not
be allocated, is dropped and counted: `sys.meta`'s `access_stats_dropped`,
beside `access_stats_shapes_per_core`. `access_statistics = off` records
nothing and allocates nothing. Measured at DA-S0 against the page it
replaced: the page-11 write cost 11.6 % of a point `SELECT`'s throughput
at `cores = 8`, outside the measurement's spread, and +0.60 us of server
CPU at `cores = 1`, inside it
(`bench/v3.4.14/results-da-s0-premise-v3.4.14-30-g673f025d.md`).

### 6.3 `sys.lock_waits` (`stats/lock_wait_log.hpp`)

One row per lock wait that parked, written when it ends: `seq`, `core`,
`started`, `waited_us`, `unit`, `mode`, `rel_oid`, `key`, `waiter_trx`,
`holder_trx` and `outcome` (`granted`, `deadlock`, `fault_net`,
`cancelled`). `unit`, `rel_oid` and `key` name the unit that refused, where
the wait is registered; `mode` is what was asked - since AY-S2 they can be
two units. `unit` is one of `relation`, `range`, `tuple`, `index key`
(the slice unit left at CP-S2a); `mode` one of `IS`, `IX`, `S`, `U`, `X`
(`U` since CP-S2b: an `UPDATE`'s version claim, and a parent `DELETE`'s
wait on a child row's writer, whose probe asks `U`, `txn.md` §5). `granted` is a flipped slot, not a grant, so a re-run that parks
again is a second row. A waiter refused at the edge for closing a cycle is
a `deadlock` row with `waited_us` 0. A path that polls a holder with no slot
behind it is no wait here (`known-gaps.md`, Locks). A ring of 4096 per
core; `lock_waits_recorded`, `_overwritten`, `_dropped`.

### 6.4 `sys.checkpoints` (`stats/checkpoint_log.hpp`)

One row per checkpoint run, a projection of BI's `CheckpointRunReport`
handed over by `Checkpointer::EndRun`; the checkpointer writes nothing else
(BI-S2's rule: no catalog page). `seq`, `core`, `started` (the wall instant
at the end less the run's length on the injected clock), `duration_us`,
`steps`, `pages_flushed`, `dirty_at_start`, `log_wait_us`, `data_wait_us`,
`resumed` and `outcome` (`completed`, `failed`). A `resumed` run took up a
failed run's snapshot and took none of its own (`dirty_at_start` 0); it can
itself fail, which is why `resumed` is a column and not an outcome. The
waits are a task run's park times plus a synchronous call's time inside the
phases that wait on the log (the gates, the end record) and on the data
file's sync (a step's, the publish's); a log gate inside the publish's
writeback and a claim wait are inside the phases that hold them and in
neither column. An abandoned task writes no row. The runs before a crash
are gone with the process; the mount's recovery block stays in `sys.meta`.
A ring of 1024 per core; `checkpoints_recorded`, `_overwritten`,
`_dropped`.

### 6.5 `sys.wait_event_names` (`stats/wait_event.hpp`)

The wait-event registry, one row per event (CJ's WE01,
`instructions/v3.2.0/workorder-cj-wait-events-ash.md`): `event_id`, `name`
(`class.event`, the only public contract), `class`, `kind` (`timed`,
`counter`), `in_db_time` (0 for `idle.*` alone) and `p1`-`p3`, what the
event's parameters mean, NULL where it has none. `event_id` is the table
index, internal and not stable across versions; 0 is the reserved
`cpu.on_cpu`. A compile-time table: no ring, no counters, nothing to
overflow. An engine-state view, reading no session view.

### 6.6 Wait statistics (`sched/wait_state.hpp`)

Cumulative, always on, memory only (CJ's WE05, WD4, `docs/spec/wait-events.md`
names every event). A core's counters have one writer, that core, and are
relaxed atomics any core reads without a latch, so a row is a moment's view:
two columns of one row may differ by a wait in flight.

- `wait_stats`: `core`, `event`, `class`, `count`, `total_us`, `max_us`. The
  `cpu.on_cpu` row is the core's ON_CPU time (count 0). `core` is NULL on the
  total rows (the sum over cores, `max_us` the largest). A synchronous read's
  time is the core's stall, not one task's (CJ-Q8).
- `wait_histogram`: `core`, `event`, `bucket`, `lo_us`, `count`; bucket k holds
  waits of [2^k, 2^(k+1)) microseconds, 32 buckets, bucket 0 everything under
  2 us.
- `session_waits`: `session_id`, `event`, `count`, `total_us`; a session's
  `cpu.on_cpu` row is its ON_CPU time and its RUNNABLE time is `sched.run_queue`.
  A session's rows leave with it; a background task is no session. **The
  sessions are listed with no latch** (§6; DL-S2): a session's words live in
  a slot that is never moved or freed, taken at its first statement and
  given back at its end by a compare-exchange, and both views walk the
  slots and read each as it stands - a slot that changed hands under the
  read is left out, and past 1,048,576 sessions at once a session is
  counted and not listed.
- `session_state`: `session_id`, `state`, `event`, `p1`-`p3`, `elapsed_us`,
  `txn_age_us`, `view_age_us`. A
  session between statements shows IDLE on `idle.client`. `session_id` is the
  engine's private counter, not the KWP handshake id. **The two ages are what
  the session holds open** (DF-Q8 (a)), which is how the session holding the
  read horizon (§7) is found: `txn_age_us` is the time since its explicit
  transaction's `BEGIN`, of either access mode, and `view_age_us` the time
  since the read view it holds was minted - its transaction's, which under
  READ COMMITTED is re-minted at each statement and under REPEATABLE READ is
  `BEGIN`'s, or its running autocommit statement's. Each is counted from the
  head of the statement that did it, on the answering core's clock; each is
  NULL with nothing held, and on an instance with no clock. An online index
  build's view is re-minted at every slice and its age follows.

**DB time** (I3) is ON_CPU + `sched.run_queue` + every timed event outside
`idle.*`; `ANALYZE` prints a statement's as `DB time 4.21 ms = CPU 1.02 +
commit.wal_flush 2.95 + ...` when the statement runs inside a task of its own
session (the sum of the parts as printed, each rounded to 10 us). A cross-core
step has no site today (every statement runs on its session's core, AT-S9), so
no cross-core line prints.

### 6.7 `sys.ash` (`sched/ash_sampler.hpp`, `stats/ash_ring.hpp`)

The active-session history (CJ's WE06-WE09). Each core's reactor samples its
own tasks once per `ash_interval_ms` (default 1000) on a drift-free grid: a
task that is not IDLE, background tasks included (`session_kind` =
`background`). A long step yields one batch at the real time with `late` = 1
(more than a tenth of an interval after its due time), nothing interpolated.
The sampler runs between polls, so `state` is never ON_CPU: CPU time reads as
RUNNABLE (CJ-Q12); `sys.wait_stats` carries the exact figure.

Columns: `sample_ts_us` (the instance's monotonic clock, not wall time), `core`,
`session_id`, `session_kind`, `state`, `event`, `class`, `sched_group`, `late`,
`arg_hash_valid` (1: `p1` is the statement's argument hash), `fingerprint` (the
statement's `fetch_id`, 0 before a statement names one and for a kind that
carries none - a `SELECT`, an `INSERT`, an `UPDATE` and a `DELETE` carry one), `p1`-`p3`,
`wait_elapsed_us`. The 64-byte record also holds `step_id`, `origin_session`
and `trx_id`, which nothing fills yet and the view does not show.
**`fingerprint` names a text, not a binding**: a sample joined to
`sys.statement_stats` by it meets every binding's row of that text (§6.1),
and nothing in the engine makes that join.

A view read copies each core's ring under that ring's own latch - the raw
records, into room made before the hold, decoded after it - so a query
during sampling sees whole records, and merges by time (ties by core, then
ring order). **The sampler takes the latch by the try form** (§6): a
sample that meets a view's copy is not taken and is counted (`sys.meta`'s
`ash_busy`), because the sampler runs on its core's reactor and a wait of
its would stop the core's statements. The ring is memory only, a fixed `ash_ring_bytes_per_core`
(default 4 MiB, 1-64 MiB, restart-only) overwritten oldest-first; a restart
begins empty. Filters are `WHERE` on the columns above.

**Settings** (`kds.conf` keys, runtime change by `SET ASH`, admin): `ash_enabled`
(on), `ash_interval_ms` (1000, 100-10000), `ash_capture_arg_hash` (off),
`ash_ring_bytes_per_core`. Off keeps the ring; on continues it. The settings are
process-wide, so two instances in one process share them.

## 7. `sys.meta`

**Tall** (BO-Q4): `(section, field, value)`, every value text, so a field
added or removed needs no schema change. The fields are the session's
core's, as `SHOW META`'s always were; the instance-wide ones read the same
on every core - among them the contention census, the four statistics
stores' counts, the backup and archive blocks, the log's durable and append
positions and roll totals, and the `load` block, which carries every
core's load. A field whose writer emits no
`=` is refused `Corruption`, never shown without a value. The field list
lives with the code that writes each section (`CommandDispatcher::MetaSections`).

**The `waystone` section** (CF-S1b, CF-S1c) carries `waystone_recording`
and `waystone_replay`, the instance's boot ceilings under their keys' own
names (`on`/`off`), and `waystone_opted`, the instance's opted count (one
load, `waystone-concpets.md` §4b). Nothing records until an operator opts
a shape in, so a config reading `waystone_recording = on` means less than it
did, and this section, the boot log line and `WAYSTONE ON`'s reply say so.

**The `join_order` section** (CX-S0, `parser-v2.md` I12) carries
`join_order`, the instance's value in force (`planned`, `fixed` or
`written` - the boot key's, or the last `SET JOIN_ORDER`'s), and the
instance's captured-plan table, the same on every core: `captured_plans`,
the shapes it holds; `captured_served`, the compiles that found a captured
order; and the planned blocks that ran without one, by reason -
`capture_deferred` (searched, not captured: a relation had no published
prior, or a DDL was open), `capture_unkeyed` (no key fits the block),
`capture_over_ceiling` (the table's ceiling) and `capture_declined` (the
captured order did not compile for the statement, or adds a sort its
`LIMIT` does not bound), the last three having run their written order.
`captured_served` counts a declined compile too: it found an order. Then `sys.plans`' side (CX-S1a): `captured_pending`, the captures waiting for the persisting task; `captured_persisted`, the rows it wrote in this run; `captured_loaded`, the captures this mount installed from the relation. And the administrator's plans (CX-S1b): `admin_plans`, the plans the instance's table holds - every one whose relations exist; `admin_plans_enabled`, how many of them are served; `plan_served`, the compiles that found an enabled plan; `plan_declined`, the variants declined for a statement, which ran its written order.

**The `txn` section** (CS-Q8 (a), `txn.md` §1) carries this core's
`read_only_begins`, the `BEGIN ... READ ONLY` transactions it opened, and
`read_only_write_refusals`, the statements it refused
`READ_ONLY_TRANSACTION` inside one (at the head or at `BeginWrite`'s second
line). Both are per core, count from the process's start, and are absent
on a dispatcher with no transaction manager. No wait event goes with them:
a read-only transaction adds no wait.

**How this core's read-write transactions ended** (DF-Q13 (a),
`stats/txn_stats.hpp`), six counts from the process's start, each counted
where its cause is known, so the six partition the transactions the core
decided:

- `commits`: a commit published - autocommit and explicit alike, a parked
  `strict` commit once its record is durable. Not a second name for the
  log's commit counts: those count records appended, by rung, and keep a
  staged `strict` commit a failed sync then aborts.
- `rollbacks`: the client's `ROLLBACK`, and a load stream's own abort. A
  `ROLLBACK` that ends nothing - after the lifetime sweep has already rolled
  the transaction back - is not counted.
- `ceiling_rollbacks`: the listener's sweep at `txn_lifetime_ceiling_ms`.
- `close_rollbacks`: a connection that went away with a transaction open,
  and a load listener detaching with a load in progress.
- `aborts`: the engine rolled the transaction back because a statement or a
  commit failed - an autocommit statement's, an autocommit or explicit
  `COMMIT`'s, a parked `strict` commit's whose sync never came, a load
  stream's failed chunk or a frame not legal inside it. A failed statement
  inside an explicit transaction decides nothing and counts nothing: the
  rollback that follows does.
- `wait_aborts`: an autocommit write gave its transaction up before it had
  written a row - to wait for a row's writer or a relation's lock, or to run
  again after a stale descent (`txn.md` §5) - counted when it is given up,
  whether the statement then runs again or is refused. One client statement
  can make several.

A `READ ONLY` transaction decides nothing and is in none of the six; nor is
the lock-holder transaction a declaration's build takes and always rolls
back, nor a mount's losers (`recovery_rolled_back`).

**The read horizon, the instance's** (DF-Q8 (a)): `read_horizon_lsn`, the
oldest snapshot LSN a live read view holds on any core, which bounds every
purge (`txn.md` §4.1); `read_horizon_core`, the core whose bound that is;
and `read_horizon_age_us`, how long ago the view holding it was minted -
the oldest such view of that core, counted from the head of the statement
that minted it. The three read the same on every core. **The statement
that reads them holds a view of its own while it runs**, so with nothing
older held it reads its own - its core, and an age near 0 - and no `SELECT`
sees the `none` the LSN and the core hold with no view held anywhere. The
age is `none` for a view nobody stamped (an instance with no clock). A KWP
load chunk's view is stamped by a clock read of its own, a chunk being no
staged statement. The three are read without a latch from words
each core publishes for itself, so they are a moment's view: a core in the
middle of minting a view has lowered its bound and not yet published the
stamp. `sys.session_state`'s `view_age_us` names the session (§6.6).

**The `connections` section** (DJ-S1, `protocol.md` §10,
`server/connection_limits.hpp`), the instance's and the same on every core:
`max_connections` and `reserved_admin_connections`, the cap and the reserve
as they apply (`0` and `0` for no limit; the reserve `0` where no gate
stamps a role); `connection_sessions`, the sessions holding a seat now, on
every core together - a session being a default-port connection from the
end of its handshake to its close, so a cancel connection, a connection
still in its handshake, a debug text session and a load session are not in
it; `connection_refusals`, the sessions the cap refused since the process
started; and
`idle_session_closes`, the default port's connections
`idle_session_timeout_s` closed (the debug text port's are closed and not
counted). Read relaxed from three words written where a session is seated,
refused or closed - a count, never a decision - and absent on a dispatcher
with no instance behind it.

## 8. What is not here

- `sys.sessions` (W2: excluded by the operator).
- Per-relation page counts and spills (BO-Q13). Delete-marked rows and
  leaf divisions, which BO-Q13 also excluded, are §6.2c's (DF-Q9 (a)).
- An exact count of a relation's delete-marked rows: §6.2c's is a sample.
- A cross-core `sys.meta`: every core's fields readable from one session
  needs a field-by-field census of what is readable across cores
  (`known-gaps.md`).
