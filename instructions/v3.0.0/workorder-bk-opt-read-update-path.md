# Work order BK — the read and update paths: less work per statement and per row

Written 2026-10-11 on `worktree-bk-read-update-paths` from `e623fb5`
(`git describe` not computed: the session's clone is shallow and does not
reach `v2.7.0`). It follows the operator's words of 2026-10-11, recorded in
English (`raft-marks-2026-10-11-bk-paths.md` §1; the day's
`raft-marks-2026-10-11.md` belongs to `workorder-bk-keystone-48.md`, the other
order of that day carrying the letter BK - BK-Q10):

- **W0:** *"Propose a direction for optimizing the read and update paths
  to be faster."*
- **W1:** *"Name the workplan BY."*, then *"No, make it BK."*

**Opened 2026-10-11 on the operator's standing go-ahead** (W2,
`raft-marks-2026-10-11-bk-paths.md` §2): *"go ahead for this milestone, keep
working until this milestone is achieved, and discard a proposal if it turns
out to be useless ... I will follow CLA's proposal if any decision needs to be
made."* Every BK-Q item, BK-Q0 included, is **adopted on the operator's
standing go-ahead of 2026-10-11 (go-ahead-achieving-milestone)** as CLA
proposed it, never marked by the operator per item; BK-Q10 was added for the
letter. W2's "discard a proposal if it turns out to be useless" is BK-R0's
rule restated: a stage whose part BK-S1 finds immaterial is not built, and
§6 records it as discarded with the number that discarded it.

**Claim tags:**
- `[source-read]` with `path:line` at `e623fb5`;
- `[measured]` with the results file it is quoted from;
- `[design]` for CLA's reasoning about code not yet written;
- `[quiet-wrong]` marks an item where a wrong choice turns a correct
  answer into a wrong one with no error.

**The letter.** `[source-read]` BJ is the last order on `main`
(`workorder-bj-expression-update.md`); no file under `instructions/` and no
branch on `origin` uses BK. W1 named it. **A second BK exists off `main`**:
`workorder-bk-keystone-48.md`, the 48-bit Keystone id, written and opened the
same day on the local branch `worktree-bk-keystone-48`. Item numbers collide
(BK-Q3 is two different questions), so this order is cited by its file name
(BK-Q10).

**BK against other orders.**
- **BA** (closed 2026-10-10) found the contended waits of P1-P4 below 1 %
  of `cores` x span in every clean cell (`workorder-ba-parallelism.md:1665-1667`).
  That is contention; it says nothing of the uncontended cost one core
  pays per statement and per row, which is BK's.
- **BI** (not opened) owns the checkpoint's reactor stall. A BK measurement
  a checkpoint lands in is re-run, not attributed.
- **BJ** (closed) owns the UPDATE row loop BK-R2 and BK-R3 change; BJ-R2's
  before-image and BJ-R9's single write hold unchanged.
- **AQ** (opened, no stage started): an expression Cabin's `F` reads columns
  outside `cabin_mask`, so BK-R2's decode set includes them.
- **CN-7** O5 (a delta log record) and BK-R5 ask one question; BK-Q6 says
  which answers it.
- **`CIP/`** (OPT-001..006) priced these paths at `v2.7.0-30..47`, before
  v3.0.0's change. OPT-001's WHERE-only first-stage decode is the precedent
  BK-R2 extends; no number of theirs is carried in (`CLAUDE.md`).

## 0. What BK is

`[measured]` Two facts frame it.

- **A statement's fixed cost is mostly outside the statement.** At BJ's
  close, one session, `relaxed`, `cores = 1`, 1,000 rows
  (`bench/v3.0.0/results-bj-close-v2.7.0-757-ge458e272.md:90-101`), p50:

  | statement | p50 |
  |---|---|
  | `SHOW META` (touches no relation) | 50.2 µs |
  | `DELETE WHERE id = k` | 48.9 µs |
  | `INSERT`, pk named | 49.2 µs |
  | `UPDATE SET v = <lit> WHERE id = k` | 51.4 µs |
  | `UPDATE SET v = v + 1 WHERE id = k` | 55.3 µs |
  | `SELECT * WHERE id = k` | 67.7 µs |

  So against `SHOW META`, a point write's parse, compile and execution are
  a few µs, and **a point `SELECT` is 17.5 µs more than a statement that
  touches nothing** - the largest engine-side share in the table. The
  ~50 µs common to all of them is the request path itself: the driver
  (`tools/kwp.py`, which sends an unnamed `C_PARSE` of literal text per
  statement, `:407`), loopback, framing and the reply.
- **A scan's per-row cost is twice PostgreSQL's.** About 70 ns a row of the
  scanned relation against 34 ns, on twelve library shapes whose geometric
  mean falls to 0.60x-0.62x at 10,000 rows unindexed
  (`bench/v3.0.0/results-kds-vs-pg18-summary-v2.7.0-545-gf2f1ee7.md` §4; the
  KDS figures describe engine `9a0525d`, `v2.7.0-531`, on another driver
  and host than the table above).

BK therefore measures first and in this order of expected size: **the
request path every statement pays, the point `SELECT`'s extra 17.5 µs, a
scan's per-row cost, then an UPDATE's row work** - and removes only what
the measurement finds material.

**BK's proposals, one line each** (each `[design]`, unratified):

- **BK-R0.** Measure first, by a bar BK-Q1 marks.
- **BK-R1.** A prepared statement is compiled once and bound per
  execution, instead of having its values substituted into its text and
  the text recompiled. `[quiet-wrong]`
- **BK-R2.** An UPDATE decodes only the columns it or a hook reads, and
  re-encodes only the cells it writes into a copy of the row's own bytes.
  `[quiet-wrong]`
- **BK-R3.** An UPDATE carries a spilled cell it does not assign into the
  new version, instead of appending a new copy - which today also leaks the
  old copy for good. `[quiet-wrong]`
- **BK-R4.** A scan's per-row cost and a point `SELECT`'s cost are split
  into parts, and the largest part is cut.
- **BK-R5.** An UPDATE's undo and WAL records carry changed byte ranges
  rather than whole row images. A format change; last, and only on BK-Q6.
- **BK-R6.** The read path's access statistics fold per core (BA-R2), only
  if BK-S1 finds them material where BA's census did not.

**BK does not do:** intra-query parallelism, a new index kind, the
checkpoint (BI) or the carve, the driver itself (BK-Q8 asks), or any change
to what a statement answers unless an item names it and is marked.

## 1. Survey at `e623fb5`

Read, not run.

### 1.1 Every execution compiles from text

- `[source-read]` A bound parameter becomes a SQL literal in the statement
  text, which the dispatcher then runs (`include/kds/server/kwp_session.hpp:82-97`;
  the substitution at `src/server/kwp_session.cpp:563`), so every `EXECUTE`
  lexes, parses and fingerprints its text again, and `UpdateInner` begins
  with `parser::Parse(line)` (`src/server/command_dispatcher.cpp:7946`).
  No compiled-plan cache exists: the catalog memo caches `TableAccess`, not
  plans.
- `[source-read]` The reason it was built so, and the condition for
  changing it: binding into a parsed statement *"needs the parser to accept
  `?` in value positions and the compiler to carry a parameter vector"*
  (`kwp_session.hpp:84-88`).
- `[source-read]` Substitution has one visible effect a binding would
  change: a string parameter containing a single quote has no spelling and
  is refused at `C_BIND` (`kwp_session.hpp:99-106`). A refusal's byte
  position also points into the substituted text.
- `[source-read]` **Only the parse leg is traced.** `trace.hpp` names
  `kParse`, `kCatalog`, `kPlan`, `kExecute`, `kHeap`, `kBufferPool`,
  `kWalAppend`, `kReply` (`include/kds/stats/trace.hpp:71-85`), but only
  `kRequest` (`command_dispatcher.cpp:980`) and `kParse` (seven sites) are
  ever opened, and tracing is per session on `TRACE ON`. BK-S1 builds the
  other spans.
- `[design]` §0's table bounds BK-R1 for a point write to the few µs a
  write costs over `SHOW META`, and the driver never exercises a
  PARSE-once, BIND-many path, so no number in the tree prices it.

### 1.2 An UPDATE decodes, encodes and logs the whole row

- `[source-read]` Per qualifying row: stage 1 decodes only the WHERE's
  columns (`command_dispatcher.cpp:8185-8194`, OPT-001); stage 2 decodes the
  whole row and resolves every spill (`:8204-8212`); the body is built from
  the whole row and encoded whole (`:8354`, `:8419`); the undo record
  carries the whole before-image (`:8460-8471`, `kOverwrite`); the WAL
  record carries the whole new image (`:8547-8556`, `kHeapOverwrite`).
- `[source-read]` Every consumer takes the row as a positional, full-width
  vector: the SET evaluator (`:8302`), the assertion reservation (`:8376`),
  the Cabin hook (`:8503`) and index maintenance (`:8510`).
- `[design]` A row is fixed-length (invariant 13; `docs/rules/rule-fixed-length-tuple.md`),
  so a cell sits at a schema-constant offset and can be re-encoded in
  place. `[quiet-wrong]` A column BK-R2 does not decode would reach a
  full-width consumer as a default `AstValue` with no error.

### 1.3 A spilled value is copied by every UPDATE of its row, and the old copy is never released

- `[source-read]` The encoder appends any value too long for its cell to
  the relation's var-heap chain (`src/exec/row_codec.cpp:228-244`). An
  UPDATE re-encodes the whole body from a row whose spills it resolved
  (§1.2), so every spilled value of the row is fetched, appended again,
  noted for undo and logged, whichever column the SET list names.
- `[source-read]` **A superseded value is not released.** What releases a
  var-heap value is rollback, of what its own transaction appended; *"a
  value whose version merely died - superseded or deleted - is not
  released; its bytes stay"* (`docs/spec/heap-and-tuple.md:97`). `PURGE`
  releases only the retired tombstone's spills. The deferred-release drain
  that would release superseded values is named and not built
  (`include/kds/txn/varheap_release.hpp:10-20`). `[design]` So today every
  UPDATE of a row with a spill leaks one copy of each spilled value for
  the life of the volume.
- `[design]` `[quiet-wrong]` Carrying the old cell (BK-R3) makes versions
  share one value. Against today's releasers that is safe - a rollback
  releases only what its transaction appended, and a carried cell appends
  nothing; `PURGE` releases only the tombstone's. What it binds is the
  future drain: a value may be released only once **the newest version
  naming it** is reclaimed. BK-Q4.

### 1.4 The per-row read cost has no breakdown

- `[measured]` About 70 ns a row against 34 (§0).
- `[source-read]` Per row a reader classifies the version - which, above
  the floor, takes `window_latch_` uncontended at one session
  (`include/kds/txn/read_view.hpp:122-132`) - decodes into `AstValue`, and
  evaluates the residual over the decoded values. BA's census measured the
  latch's *contended* wait, so its uncontended cost is inside the 70 ns and
  unpriced.
- `[measured]` BJ's close could not attribute a residual cost because
  `perf` is unavailable on the measuring host
  (`workorder-bj-expression-update.md:1135`). Timing four parts of a 70 ns
  row with a clock read each would perturb what it measures, so BK-S1
  splits by sampled timing or by differential builds (a part removed or
  doubled), not by per-row clocks.

### 1.5 Access statistics on every SELECT

- `[source-read]` A SELECT records its access shapes (`RecordAccessShapes`,
  `command_dispatcher.cpp:7539-7543`), and `Catalog::RecordAccess` holds
  `sys.access_stats`' root page exclusive for its body (`docs/spec/crosscore.md`
  CC13). CC13's "+1-2 % on a point lookup" cites `heap-and-tuple.md` §7,
  which does not carry it: the number is unsourced and may predate v3.
- `[measured]` BA's census found P3, which includes page 11, below 1 % as a
  contended wait (`workorder-ba-parallelism.md:1665-1667`); that is why
  BA-S5 did not open.

## 2. Rulings — CLA's proposals, not marked

### BK-R0 — Measure first

BK-S1 builds the spans and counters and runs the census; a fix stage opens
only for a part BK-Q1's bar finds material. BA-R0's bar, for comparison,
is *"an item is material when its share reaches 5 % in any cell, or when
it refused anything"*, its share being a contended wait over reactor wall
time (`workorder-ba-parallelism.md:741-745`); BK measures uncontended time,
so it needs a bar of its own.

### BK-R1 — Compile once, bind many `[quiet-wrong]`

- **The key is the statement, not its shape.** A compiled plan is cached on
  the KWP statement handle (`C_PARSE`), with the `?` text and each
  parameter's type, never by `pattern_id`: `pattern_id` merges an integer
  and a string literal by design (`include/kds/parser/fingerprint.hpp:65-70`)
  and is collision-survivable only because a Waystone replay is validated
  (`:89-100`), which a plan has no counterpart for. The fingerprint of a
  bound execution must still equal the substituted one's.
- **The plan holds no catalog borrow** (`catalog.md` CT2): each execution
  re-binds its relations, as the step VM's re-bind does, and a moved
  schema version word recompiles. The word also moves on every tree root
  move, so BK-S1 counts how often it moves under the bench load.
- **Re-made per execution, never cached:** every compile decision that
  reads a value or the clock - a literal's coercion to its column, a pk
  point's or key window's bounds (BH's past-int64 window defect is the
  example), a probe's key value, `LIMIT`/`OFFSET`, BJ-Q3 (c)'s bare-literal
  gate, BJ-Q10's constant fold, the foreign-key parent hoist, the
  `StatementContext` (`NOW()`) and the `ReadView`. BK-S2 lists them from
  the compiler before it builds, and the list is reviewed: a decision it
  misses is the quiet-wrong case.
- **Two answers change** and are marked before they do (BK-Q3): a quoted
  string parameter becomes bindable; a refusal's byte names a position in
  the `?` text.

### BK-R2 — Decode what is read, encode what is written `[quiet-wrong]`

- Stage 2 decodes the union of: the columns the SET list reads and assigns,
  and the columns a Cabin (AQ's `F` included), an index or an assertion on
  the relation reads.
- An undecoded slot carries a poison value that a consumer reading it
  refuses `Corruption` under an assert, never a default value.
- The new body is the row's own bytes with the assigned cells re-encoded
  in place, the NULL bitmap's bits for them set, and - **until BK-R3 lands
  - every spilled cell re-encoded as today**, so BK-R2 changes nothing about
  var-heap sharing. A cell asserts the new image equals today's whole-row
  encode for every type and NULL, spills excluded.

### BK-R3 — An unassigned spill is carried, and the leak stops `[quiet-wrong]`

An unassigned spilled cell is copied into the new body as it stands,
pointing at the same immutable value (invariant 14). On BK-Q4's mark, which
also records the binding rule on the future drain (§1.3). Its cells: a
rollback of the carrying version leaves the value; a `PURGE` of a row whose
older version carried a value releases it once and only once; recovery's
undo phase likewise.

### BK-R4 — Where a read's time goes, and the largest part cut

BK-S1 splits a point `SELECT`'s 17.5 µs over `SHOW META` and a scan row's
70 ns into parts - compile, the descent, classification (the uncontended
latch included), decode, residual, reply encoding, access statistics. The
fix stage is the part BK-S1 ranks first (BK-Q5).

### BK-R5 — Byte-range undo and WAL records

A record shape for an overwrite whose changed ranges are smaller than the
row. Its readers are every reader of a before-image, not only rollback:
MVCC visibility replaces the payload from it (`src/txn/visibility.cpp:20-32`),
and so do the foreign-key reverse walk, the live `Compensate` and
recovery's undo; each applies ranges newest-first. Redo applies after-ranges.
A WAL and undo format change and a superblock bump under the standing
order of 2026-10-07. Last, and only on BK-Q6.

### BK-R6 — Access statistics fold per core

BA-R2 as written, only if BK-S1 finds `RecordAccess` material. BA-R2
carries BA-Q4's freshness change - counts up to 100 ms late, CC13's
sentence struck - which changes `SHOW ACCESS`'s answers and needs its own
mark (BK-Q7).

## 3. Stages

| stage | what | done when | size |
|---|---|---|---|
| BK-S0 | **The order** | This file, its review, `raft-marks-2026-10-11.md`, `index.md`'s row | S |
| BK-S1 | **The spans, the counters, the census** (BK-R0) | The trace spans `trace.hpp` names and nobody opens, built; a sampled or differential split of a point `SELECT` over `SHOW META`, of a scan row, and of a point `UPDATE`; the request path every statement pays split from the server's side; the schema word's moves per second under load; the driver able to PARSE once and BIND many, both paths measured; `build-release`, a quiet host, results under `bench/v3.0.0/`. No path changes | L |
| BK-S2 | **Compile once, bind many** (BK-R1), if material and on BK-Q3 | The cache on the statement handle; re-bind per execution; the reviewed list of per-execution decisions with a cell each; fingerprint equality for every bound shape; a DDL and a root move between two executions | L |
| BK-S3 | **Decode and encode by column** (BK-R2), if material | The decode set, the poison slot, the in-place re-encode; byte equality with today's encode across every type and NULL; BJ's single-write and swap cells green | M |
| BK-S4 | **The read path's largest part** (BK-R4), if material | The part BK-S1 ranks first, answers byte-identical | M |
| BK-S5 | **Carried spills** (BK-R3), on BK-Q4 | The carry, its rollback, purge and recovery cells; the drain's binding rule written into `heap-and-tuple.md` | M |
| BK-S6 | **Access statistics per core** (BK-R6), if material and on BK-Q7 | BA-S5's done-when | S |
| BK-S7 | **Byte-range records** (BK-R5), on BK-Q6 | The record, every before-image reader, redo, undo, the superblock bump, the sim's crash cells | L |
| BK-S8 | **The close** | §5's measurement; the specs each stage changed | S |

**Order.** S1 first; then the fix stages in the order BK-S1 ranks them.
S5 waits on BK-Q4 alone, since it fixes a leak whatever the census says;
S7 last.

## 4. Items for the operator

| item | question | kind | CLA's proposal | mark |
|---|---|---|---|---|
| BK-Q0 | **Open BK**, with BK-S0..S8 and BK-R0..R6 as written | process | Yes | adopted (standing go-ahead, 2026-10-11) |
| BK-Q1 | **The materiality bar** for a fix stage | process | **CLA's, modelled on BA-R0**: a part is material when it is at least 5 % of its statement's or its row's server-side time in a clean cell, and the fix's predicted win exceeds the cell's run-to-run spread | adopted (standing go-ahead, 2026-10-11) |
| BK-Q2 | **The plan cache's scope**: (a) per session; (b) per core, shared by sessions | design | **(a).** Nothing is shared and nothing needs a latch; (b) only if BK-S1 shows many sessions preparing one statement | adopted (standing go-ahead, 2026-10-11) |
| BK-Q3 | **The two answers BK-R1 changes**: a quoted string parameter bindable; a refusal's byte in the `?` text | user-visible | **Both accepted**, each stated in `protocol.md` and the manual; the first removes a refusal that exists only because of substitution | adopted (standing go-ahead, 2026-10-11) |
| BK-Q4 | **A var-heap value two versions share** (BK-R3): (a) carry it, and bind the future drain to release a value only when the newest version naming it is reclaimed; (b) a per-value reference count; (c) not built | design, **[quiet-wrong]** | **(a).** Safe against every releaser that exists (§1.3), no new on-disk field, and it stops a leak that grows with every UPDATE of a spilled row. (b) is a format change for a guarantee (a) gives by a rule | adopted (standing go-ahead, 2026-10-11) |
| BK-Q5 | **Which read part BK-S4 cuts** | design | Whatever BK-S1 ranks first | adopted (standing go-ahead, 2026-10-11) |
| BK-Q6 | **Byte-range undo and WAL records** (BK-R5), here or as CN-7's O5 | format | **Here, only if BK-S1 finds a record's size or copy material, after BK-S3**; otherwise not built and CN-7 O5 stays open | adopted (standing go-ahead, 2026-10-11) |
| BK-Q7 | **BA-R2's freshness change** under BK-R6: counts up to 100 ms late | user-visible | Only if BK-S6 opens; then yes, with CC13 restated | adopted (standing go-ahead, 2026-10-11) |
| BK-Q8 | **The request path's ~50 µs** if BK-S1 puts it in the driver or the client rather than the server | scope | **Out of BK**: reported, and its own order if wanted, since no engine change reaches it | adopted (standing go-ahead, 2026-10-11) |
| BK-Q9 | **The measurement** (§5), at BK's close | process | As written | adopted (standing go-ahead, 2026-10-11) |
| BK-Q10 | **The letter**, which `workorder-bk-keystone-48.md` also carries: (a) keep BK, cite by file; (b) re-letter this order | process | **(a).** W1 names BK after rejecting BY, so the letter is the operator's; `index.md` already carries a letter in two documents (AN) under the rule "cite the file, never the bare number"; this order's marks go to `raft-marks-2026-10-11-bk-paths.md` so the two sessions do not write one file | adopted (standing go-ahead, 2026-10-11) |

## 5. Measurement

At BK-S1 (the census) and once at the close, `build-release`, a quiet host,
interleaved, A = the commit BK opens from, B = the commit that closes it,
each named by `git describe --tags` after the clone is deepened to
`v2.7.0`, results under `bench/v3.0.0/`.

- Point `SELECT`, point `UPDATE` (literal, `v + 1`), `INSERT`, `DELETE` by
  pk, and `SHOW META` as the floor, one session, `relaxed`, each over both
  driver paths: literal `C_PARSE` per statement, and PARSE once, BIND many.
- A 10,000-row unindexed equality against BA-S3's PostgreSQL twin, so §0's
  70 against 34 ns is re-read on one driver and one host.
- A point UPDATE of a row with a spilled `varchar`, assigning another
  column, with the var-heap's page count before and after N updates.

## 6. Row status

### BK-S0 — written 2026-10-11, opened the same day

**Opened** on `worktree-bk-read-update-paths` from `ac04b84e` (`main` at the
time, `e623fb5` plus the negative-foreign-key fix): the order committed with
`raft-marks-2026-10-11-bk-paths.md` and `index.md`'s row, every item adopted on
the operator's standing go-ahead of 2026-10-11 (go-ahead-achieving-milestone),
BK-Q10 added for the letter. Before adopting BK-Q4 `[quiet-wrong]` its premise
was re-read at `ac04b84e`: a var-heap value is released by exactly three
callers of `txn::ReleaseVarHeapSlot` - a live rollback of its own transaction's
appends (`src/txn/manager.cpp:436`), recovery's undo of the same
(`src/txn/recovery_undo.cpp:51`), `PURGE` of the retired tombstone's own cells
(`src/exec/purge_key.cpp:41`) - plus the catalog relations' sweep
(`src/exec/varheap_sweep.cpp:46`), which `UpdateInner` never writes. None
releases a value it did not append or a tombstone did not name, so the premise
holds.

Written on `worktree-bk-read-update-paths` from `e623fb5`. No file under
`src/`, `include/` or `tests/` moved, and no suite ran.

**Its review** (one independent pass against the tree at `e623fb5`) found
17 items, all applied:
- **§0 rebuilt on BJ's close file**: `SHOW META`, which touches no
  relation, costs 50.2 µs, so a point statement's fixed cost is mostly the
  request path, and the point `SELECT`'s 17.5 µs over it is the largest
  engine share.
- **§1.3**: a superseded spill is never released, so the copy is also a
  leak; the release hazard restated against the releasers that exist, and
  BK-Q4's rule turned the right way (the newest version naming a value,
  not the oldest).
- **BK-R2 no longer carries spills**, so it does not build BK-R3 without
  BK-Q4; it is tagged `[quiet-wrong]` for an undecoded slot.
- **BK-R1** keyed on the statement handle, not `pattern_id`; no catalog
  borrow across a boundary; the per-execution decisions extended; the two
  answer changes named (BK-Q3).
- **The bar** is CLA's, beside BA-R0's quoted text.
- **The trace**: only the parse leg is opened; BK-S1 builds the rest, sized
  L, by sampled or differential timing.
- **§1.4, §1.5**: the census measured contention, not uncontended cost;
  CC13's "+1-2 %" is unsourced; BA-R2's freshness change named (BK-Q7).
- **BK-R5** lists every before-image reader.
- **§5**: the driver's literal-PARSE path and a PARSE-once path both.
- Citation precision and the summary's 0.60x as a geometric mean on
  `9a0525d`.
