# Work order BK — the read and update paths: less work per statement and per row

Written 2026-10-11 on `worktree-bk-read-update-paths` from `e623fb5`
(`git describe` not computed: the session's clone is shallow and does not
reach `v2.7.0`). It follows the operator's words of 2026-10-11, recorded in
English (`raft-marks-2026-10-11-bk-paths.md` §1). The day's
`raft-marks-2026-10-11.md` is two files that will meet: `main`'s, written at
`ab2c46d0` for `workorder-bk-sys-statistics-show-sugar.md` and BL and merged
into this branch at `8679367e`, and `worktree-bk-keystone-48`'s, for
`workorder-bk-keystone-48.md` - the two other orders of that day carrying the
letter BK (BK-Q10):

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
the order records it as discarded with the number that discarded it (§2's
ruling, §4's item, §6's entry).

**Closed 2026-10-11** on `worktree-bk-read-update-paths` (code at `8679367e`,
`v2.7.0-787-g8679367e`; not pushed): BK-S1, S3, S4, S5, S9 built; BK-S2, S6,
S7 not built, each with its number (§2's BK-R1, BK-R5, BK-R6, §4's BK-Q13,
§6's "not built" entry); measured once at the close
(`bench/v3.0.0/results-bk-close-v2.7.0-787-g8679367e.md`).

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
same day on the local branch `worktree-bk-keystone-48`, and - found when
`origin/main` was merged at BK's close - `workorder-bk-sys-statistics-show-sugar.md`
(statistics relations and `SHOW` as sugar), written the same day and on
`main`. Item numbers collide (BK-Q3 is three different questions), so this
order is cited by its file name (BK-Q10).

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

**BK's proposals, one line each** (each `[design]`; adopted on the standing go-ahead, §4):

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
  if BK-S1 finds them material where BA's census did not. **Discarded at
  BK-S1**: 0.7 % of a point `SELECT`.
- **BK-R7** (added at BK-S1). A statement's reply leaves in one write: the
  frames a pipelined PARSE/BIND/EXECUTE earns are flushed at its completion,
  not partly when the read submits it.
- **BK-R8** (added at BK-S1). A var-heap append starts its walk at the
  chain's last known tail, not at its root.

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

**Not built in BK** (BK-Q13, adopted at BK-S3): after BK-S4 and BK-S5 the
part BK-R1 removes - parse and compile - is 1.1-4.5 µs of a point
statement's server CPU, 5-15 % of it (B's profile at the close,
`bench/v3.0.0/archive/bk-close-v2.7.0-787-g8679367e/partsB.md`), reachable
only by a client that prepares.

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

**As built** (BK-S5 built the encode half, `exec::EncodeAssigned`; BK-S3 the
decode half). **Revised in one point**: a relation with any hook - a Cabin,
an index, an assertion - decodes its row whole, rather than the union of the
columns each hook reads. Each hook takes the row full-width
(`NoteCabinWrite`, `MaintainIndexes`, `AdmitAndReserveUpdate`), so a union
would put `kUndecoded` in front of three consumers whose every read would
then need proving; whole-where-hooked leaves the SET evaluator the only
reader of a partial row, and it refuses the poison `Corruption`. The decode
a relation with no hook skips is the one the census measured.

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

**Discarded at BK-S3's re-profile**: an every-row UPDATE's undo and WAL work
is per record, not per byte - an `UNDO_WRITE` and a `HEAP_OVERWRITE` a row,
the undo log's append 28 % of a row and the WAL append 7.5 %, for a 44-byte
image. BK-Q6's condition, a record's size or copy material, is not met;
BK-S7 does not open and CN-7's O5 stays open.

### BK-R6 — Access statistics fold per core

BA-R2 as written, only if BK-S1 finds `RecordAccess` material. BA-R2
carries BA-Q4's freshness change - counts up to 100 ms late, CC13's
sentence struck - which changes `SHOW ACCESS`'s answers and needs its own
mark (BK-Q7). **Discarded at BK-S1** (`bench/v3.0.0/results-bk-s1-census-v2.7.0-769-gb70f79d8.md`
§3): `Catalog::RecordAccess` is 0.7 % of a point `SELECT`'s server CPU, under
the bar; BK-S6 does not open and BK-Q7's freshness change is not made.

### BK-R7 — A statement's reply leaves in one write (added at BK-S1)

`[measured]` The census's largest part of every point statement is the
socket send, 38-46 % of its server CPU, and it splits evenly between two
flushes: `TcpServer::OnClientReadable` sends `S_PARSE_OK`/`S_BIND_OK` as soon
as the read has submitted the statement, and `OnStatementComplete` sends the
rest. Two `send()` calls and two client wakeups per statement, the first
carrying nothing a client acts on. **The ruling**: while a statement this
read submitted is in flight, the read does not flush; the completion
flushes everything queued, in order. No frame changes and no frame moves
relative to another; what changes is that the first two leave with the
rest. A client that blocks reading `S_PARSE_OK` before sending its EXECUTE
never had a statement in flight, so it is answered at once as before. Given
to BK-S4 by BK-Q5 (the part BK-S1 ranks first), so it needs no item of its
own.

### BK-R8 — A var-heap append starts at the chain's tail (added at BK-S1)

`[measured]` `varheap::ChainAppend` walks the relation's chain from its root
to the tail on every append (`src/storage/varheap.cpp:325-348`), so each
spilled value written costs a page fetch per page the chain already has.
The census put the walk at 32-60 % of an UPDATE assigning a spilled value
after 30,000 updates of that relation, and growing; it is the same walk
under every INSERT of a spilled value. **The ruling**: the append starts
from the chain's last known tail, a hint kept per relation in memory and
validated before use - a `kVarHeap` page stamped with the relation's oid
(`page.md` §2a) - falling back to the root when it is not one. The walk's
existing re-check (a tail that links on is not the tail) is what makes a
stale hint safe: it only ever moves the start forward along the same
chain. Nothing on disk changes. In BK's scope by BK-Q12.

## 3. Stages

| stage | what | done when | size |
|---|---|---|---|
| BK-S0 | **The order** | This file, its review, `raft-marks-2026-10-11-bk-paths.md`, `index.md`'s row | S |
| BK-S1 | **The spans, the counters, the census** (BK-R0) | The trace spans `trace.hpp` names and nobody opens, built; a sampled or differential split of a point `SELECT` over `SHOW META`, of a scan row, and of a point `UPDATE`; the request path every statement pays split from the server's side; the schema word's moves per second under load; the driver able to PARSE once and BIND many, both paths measured; `build-release`, a quiet host, results under `bench/v3.0.0/`. No path changes **Revised by BK-Q11**: neither the spans nor the schema word's rate built | L |
| BK-S2 | **Compile once, bind many** (BK-R1), if material and on BK-Q3. **Not built** (BK-Q13) | The cache on the statement handle; re-bind per execution; the reviewed list of per-execution decisions with a cell each; fingerprint equality for every bound shape; a DDL and a root move between two executions | L |
| BK-S3 | **Decode and encode by column** (BK-R2), if material; **widened at BK-S3 to the every-row UPDATE's per-row parts** (BK-Q13) | The decode set, the poison slot, the in-place re-encode; byte equality with today's encode across every type and NULL; BJ's single-write and swap cells green | M |
| BK-S4 | **The read path's largest part** (BK-R4), if material | The part BK-S1 ranks first, answers byte-identical | M |
| BK-S5 | **Carried spills** (BK-R3), on BK-Q4 | The carry, its rollback, purge and recovery cells; the drain's binding rule written into `heap-and-tuple.md` | M |
| BK-S6 | **Access statistics per core** (BK-R6), if material and on BK-Q7 | BA-S5's done-when. **Discarded at BK-S1** (BK-R6) | S |
| BK-S7 | **Byte-range records** (BK-R5), on BK-Q6. **Discarded** (BK-R5) | The record, every before-image reader, redo, undo, the superblock bump, the sim's crash cells | L |
| BK-S8 | **The close** | §5's measurement; the specs each stage changed | S |
| BK-S9 | **The var-heap tail** (BK-R8), added at BK-S1 on BK-Q12 | The hint, its validation, its fallback; an append from a stale or foreign hint lands on the real tail; a chain grown by two cores at once stays one chain; the census's assigned-spill arm flat in the chain's length | M |

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
| BK-Q11 | **BK-S1's two instruments the census replaced** (added at BK-S1): the trace spans `trace.hpp` names, and the schema word's rate under load | process | **Neither built.** The sampler (`tools/kds_sampler.c`) splits a statement's server CPU into the parts BK-R4 asks for without code under `src/` and without per-row clocks (§1.4); the schema word's rate prices only a cached *plan*'s invalidation, and BK-R1's form is decided after BK-S4 and BK-S5 move the denominators | adopted (standing go-ahead, 2026-10-11) |
| BK-Q12 | **BK-R8 in BK** (added at BK-S1): the var-heap append's walk from the root is per-row work on the update path the census measured, but no ruling of BK named it | scope | **In BK, as BK-S9.** It is the update path's largest part for a row whose spilled cell is assigned once BK-R3 lands, it grows without bound, and it changes nothing on disk or on the wire | adopted (standing go-ahead, 2026-10-11) |
| BK-Q13 | **What BK-S2 and BK-S3 build, after BK-S4 and BK-S5 moved the denominators** (added at BK-S3) | scope, [quiet-wrong] for (a) | Options for BK-S2: (a) BK-R1 as written, a plan cached on the statement handle; (b) a reduced form, the parse cached and the compile per execution; (c) not built in BK. For BK-S3: the decode half of BK-R2 as written, or widened to the every-row re-profile's per-row parts. **CLA's proposal: BK-S2 (c); BK-S3 widened.** BK-S2: parse and compile are 1.1-4.5 µs of a point statement's server CPU after BK-S4 (5-15 %, B's profile at the close), material by the bar's letter, but reachable only by a client that prepares - no driver in the tree does - and 2.5-10 % of the statement a client waits for (B's p50 at the close); either form puts BK-R1's own quiet-wrong surface (every per-execution decision, the fingerprint's argument stream, every DML handler's entry) on the parser and the dispatcher. W2 asks that a proposal that does not pay be discarded; it is recorded in `known-gaps.md` with the census's numbers. BK-S3: the re-profile's per-row parts are the undo log's second fetch of its tail (7 %), the whole-row decode (6.6 %) - both in `archive/bk-s1-census-v2.7.0-769-gb70f79d8/profB1r-reports.txt` - and, of the read path, `ExecStats::For` out of line (5.5 % of a scan row) - each at or over the bar, each a change no answer can see | adopted (standing go-ahead, 2026-10-11) |

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

### BK-S1 — the census, 2026-10-11

Committed `0d124dec`.

On `worktree-bk-read-update-paths`, A = `b70f79d8` (`v2.7.0-769-gb70f79d8`):
`bench/v3.0.0/results-bk-s1-census-v2.7.0-769-gb70f79d8.md`. Built, all under
`tools/`, nothing under `src/`: `kds_sampler.c` (an `LD_PRELOAD` SIGPROF
sampler - `perf` is refused on the host, `perf_event_paranoid = 4`) with
`kds_sampler_report.py` and the part list `bk_parts.json`; `bk_census.py`
(per statement, the client's wall time and both processes' on-CPU time, both
driver paths, an interleaved A/B mode for the close) with `bk_census_table.py`;
`kwp.py`'s `prepare`/`execute_prepared`, the PARSE-once path. **The host was
busy** (loadavg 13-20, other sessions' builds in every run), so the census
quotes on-CPU time and sampled shares of it, never wall time.

**What it ranked**: the socket send, 38-46 % of every point statement, half of
it a second flush (BK-R7, added; BK-S4 by BK-Q5); the spilled row's re-append
and the walk from the chain's root it lengthens - 37-54 % of an UPDATE of a
spilled row assigning another column, 32-60 % of one assigning the spilled
value, each growing with the chain - and 38 pages leaked per 1,000 updates (BK-R3, BK-S5; BK-R8,
added, BK-S9 by BK-Q12); parse + compile 5-12 % of a point statement (BK-R1,
form decided after BK-S4/S5); Waystone recording 7-9 % of a point `SELECT`
(no BK proposal; reported); access statistics 0.7 % (**BK-R6 discarded**). The
every-row UPDATE's and the scan row's split were too coarse in that pass (92-99
and 450-526 samples) and are re-profiled. BK-Q11 adopted: the trace spans and
the schema word's rate were not built.

### BK-S4 — a statement's reply in one write (BK-R7), 2026-10-11

Committed `fd851d32`.

`src/server/tcp_server.cpp`: `OnClientReadable` does not flush while the
statement it submitted is in flight; `OnStatementComplete` flushes all of it.
A `reply_writes()` count on `TcpServer` (reactor-thread state, read after the
reactor stops) and the cell `KwpEndpointTest.APipelinedStatementRepliesInOneWrite`
(three writes for a handshake, a statement and STOP, each one client write;
five before - the cell was run against the change reverted and read 5).

### BK-S5 — carried spills (BK-R3), 2026-10-11

Committed `ed4a5655`.

`exec::EncodeAssigned` (`src/exec/row_codec.cpp`): an UPDATE's new image is the
row's own bytes with only the assigned cells zeroed, their NULL bits cleared
(`catalog::ClearNullBit`, new beside `SetNullBit`) and encoded as `EncodeRow`
encodes them; every other cell, spilled or not, is carried. `EncodeRow` and it
share `EncodeColumn`/`StartRow`. `UpdateInner` encodes through it from
`payload_copy`, and its undo before-image is `payload_copy` too, where it was
re-read from the page (one fetch and one copy a row fewer; the walk holds the
page's write `PageRef` and does not suspend, the premise the conflict check
already states). The release rule BK-Q4 (a) binds is written into
`heap-and-tuple.md` §3.4, `rule-fixed-length-tuple.md`, `varheap.hpp` and
`varheap_release.hpp`. Cells: `RowCodecEncodeAssignedTest` (byte equality with
`EncodeRow` for every column of every type, NULL both ways, a shorter string
over a longer, every column at once; two mutants - the cell not zeroed, the bit
not cleared - each fail it), `VarHeapLifetimeTest`'s premise rewritten
(`AnUpdateOfAnotherColumnCarriesTheSpilledValue`, with a repeatable-read reader
holding the old version through it) and two rollback cells,
`varheap_carry_crash_test.cpp` (a carrying UPDATE cut at every record it logs,
an undecided one undone at mount, a `PURGE` after carries), and
`FixedLengthTupleTest.AStoredValueSurvivesAnUpdateOfAnotherColumn` extended with
`SET d = d, p = p`, the one route left to the decode-encode round trip.

**The review of BK-S4 and BK-S5** (one `critics-developer` pass over the diff):
no correctness defect in either. Applied: the release rule stated wrongly in
`varheap.hpp` (it still read "when a version dies its values die with it") and
in `varheap_release.hpp`'s drain sentence; BK-R7 cited from the code before the
order carried it (this entry and §2 now do; the code keeps only the citation);
`heap-and-tuple.md` §3.4's "what releases is rollback" missing `PURGE`; a second
writer of the NULL bitmap (now `ClearNullBit`); three stale comments and the
fixed-length cell's premise; the undo image from `payload_copy`; the two loops
over the assignments merged; an unreachable null-pointer test dropped; the
write count moved by the move operations like every other member; the
one-write cell's flake mode named; the purge cell renamed (`PageRelease` is
idempotent, so "once" was not observable). Rejected: none.

Overhead not measured; measured at the milestone's close.

### BK-S9 — the var-heap tail (BK-R8), 2026-10-11

Committed `e6b320b4`.

`varheap::ChainAppend` starts its walk at the chain's last known tail: a
1,024-slot table of relaxed atomics on `PageStore` (`root << 32 | tail`, one
table per store, so per instance in production, where peers share core 0's
store), written when an append lands somewhere new and **forgotten when a root
is created** (`CreateChain`), so every hint is a page of its own chain; the
hinted page's type and owner stamp are checked anyway, and anything else - a
failed fetch included - sends the walk to the root, which is the authority.
The re-check under the tail's hold is unchanged. `heap-and-tuple.md` §3.4 and
`rule-fixed-length-tuple.md` state it. Cells (`VarHeapChainTest`): an append
starts at the last tail and never reads the root; a hint the chain moved past
is walked forward; a hint at another chain's page, a heap page or a page never
created sends the walk to the root and writes no other chain; a root is born
with no hint; two growers from a hinted start stay on one chain. Two mutants -
the hint never used, the owner not checked - each fail a cell.

**Its review** (one `critics-developer` pass): **one finding that changed the
design** - a hint survives `DROP TABLE` within a run, a reclaimed root id can
be reused as a new chain's root, and the stale tail could then be any page of
any relation, latched shared under the caller's held leaf: a page pair BB-R4's
census does not cover. No value lands on a wrong chain (the owner check), but
the latch pair is new. Applied: `CreateChain` resets the root's slot, so a
hint is only ever a page an append found on this chain, and the walk latches
only what the walk from the root would. Also applied: the hint written only
when it moves (a hot relation's appends no longer store the same word into a
line every core reads); the type test as `RawPageType` (no `Status` built);
the hash width a named constant; the four restatements of the trust argument
cut to one; one cell's comment narrowed to what it pins. Rejected: the
fallback spending one step of the cycle budget (a chain of exactly
`kMaxChainPages` pages; left as is).

### BK-S3 — the UPDATE row's per-row work (BK-R2, widened by BK-Q13), 2026-10-11

Committed `22f07942` and `3a5081e5`.

Three cuts, from the every-row UPDATE's and the scan's re-profile
(the B1 profile - the tree committed as `fd851d32` and `ed4a5655` - with
1,500 statements of 1,000 rows: `bench/v3.0.0/archive/bk-s1-census-v2.7.0-769-gb70f79d8/profB1r-reports.txt`):

- **Stage 2 decodes what is read** (BK-R2's decode half, as revised): a
  relation with a Cabin or an index, or one the assertion registry answers for,
  decodes the row whole; any other decodes only the columns its SET
  expressions read (`exec::SetExpressionColumns`), into a statement-scoped row
  whose other slots hold `exec::kUndecoded`, which the SET evaluator refuses
  `Corruption`. `previous` and the assigned values written into the row exist
  only where a hook reads them.
- **The undo log appends through the hold that checked the tail's room**
  (`UndoLog::TailFor` returns the held `PageRef`), not through a second fetch
  of the same page - every write statement's undo record, not only UPDATE's.
- **`ExecStats::For` inlines** (`3a5081e5`), and a walk's examined row takes
  its counters once: the read path's own per-row cut, 5.5 % of a scan row in
  the same profile. Its review (one `critics-developer` pass) found the move
  correct, the stage tag wrong and a second lookup per row removable; both
  applied.

Cells: `SetExprEvalTest.APartialDecodeReadsEveryColumnTheExpressionsName`
(CASE, COALESCE, CAST, LENGTH, GREATEST, a subquery, and a literal-only
statement between two that read), `...AnIndexedRelationDecodesTheKeyColumnsTheSetListDoesNotRead`
(a composite key whose other column the SET list does not read), and
`SetExprUndecodedTest.AnExpressionReadingAnUndecodedSlotIsRefused`. Three
mutants: no relation hooked (Cabin and index cells fail), the assertion ask not
widening the decode (the assertion cells crash - an assertion hook handed
`kUndecoded` is not a refusal, which is why the rule is whole-where-hooked),
and a column reference adding nothing to the mask (eight expression cells
fail).

**Its review** (one `critics-developer` pass): **one regression, fixed** - the
first draft asked the assertion registry before the first-updater-wins block,
which is where the row's writer takes the relation's `IX`; a `CREATE ASSERTION`
build adopts under `X`, so a build adopting between the ask and the grant
would have left the row written unchecked. The decode and the ask now come
after the conflict block, as the ask always did; no cell drives that race (it
needs a refused `IS`, an adoption on another core and a grant in one window).
Also applied: the duplicated and stale "asked once" comment; the `Tail` struct
replaced by the `PageRef` it carried (`PageRef::page_id()`); the poison state
as one branch; the `kUndecoded` comment no longer claiming the encoder sees
one; a cross-reference from `hooked` to the two early returns it negates; the
direct evaluator cell. Noted, not changed: a whole-decoded slot's
`byte_offset`/`scale`/`dec_hi` are no longer reset by a fresh vector between
rows - every assigned column is reassigned before a hook reads it, and no
reader compares those fields.

Full Debug suite green at the end of both stages (3518/3518). Overhead not
measured; measured at the milestone's close.

### BK-S8 — the close, 2026-10-11

**Measured once, interleaved, on a quiet host** (1-minute loadavg 2.6-3.7 at every run's start of the main A/B, 1.1-1.4 for DELETE's and the PostgreSQL twin; a gate at 4):
A = `b70f79d8` (`v2.7.0-769-gb70f79d8`), B = `8679367e` (`v2.7.0-787-g8679367e`,
BK's code merged with `origin/main`; later commits are documents),
`bench/v3.0.0/results-bk-close-v2.7.0-787-g8679367e.md`. Eight A/B runs of every
shape and both driver paths, six more for DELETE, B's sampled profile, and the
PostgreSQL twin of the scan. At p50, B - A, the median of the per-run
differences:

| shape | A p50 | B - A | server CPU, B - A |
|---|---|---|---|
| `C_PING` (control, no engine path) | 26.2 µs | +0.0 µs | +0.0 µs |
| `SHOW META` | 50.5 µs | -4.4 µs | -8.8 µs |
| point `SELECT` | 65.5 µs | -9.2 µs (-14 %) | -7.4 µs |
| point `UPDATE`, literal / `v + 1` | 53.8 / 56.6 µs | -7.9 / -8.5 µs (-15 %) | -7.5 / -6.4 µs |
| point `UPDATE` of a spilled row, cell not assigned | 62.3 µs | -16.6 µs (-27 %) | -16.0 µs |
| point `UPDATE` assigning a spilled value | 65.0 µs | -14.1 µs (-22 %) | -12.2 µs |
| `INSERT` / `DELETE` by pk | 52.4 / 50.1 µs | -7.2 / -7.6 µs (-14 / -15 %) | -6.4 / -6.7 µs |
| every-row `UPDATE`, 1,000 rows | 987 µs | -228 µs (-23 %) | -221 µs |
| unindexed equality, 10,000 rows | 783 µs | -38 µs (-5 %) | -37 µs |

The prepared path moves the same way (its rows are in the results file).
1,000 updates of a spilled row, assigning another column, allocated 38
var-heap pages on A and 0 on B in every run; an UPDATE assigning a spilled
value costs the same in the arm's last block as in its first on B, where A's
grows with the chain. A scan row is 67 ns of server CPU net of the statement
on B, 69.5 on A, and **34 on PostgreSQL 18** (BA-S3's twin, one host, one
driver): §0's "twice PostgreSQL's" stands after BK, and is `known-gaps.md`'s.

**Where it came from**, by B's profile against A's: the one write is most of
every point statement's gain (one `send()` instead of two, and one client
wake); the carried spill and the tail hint are the spilled rows'; the every-row
UPDATE's is the undo log's single fetch, the before-image taken from the walk's
copy, the partial decode and no whole-row encode. The results file splits it
as far as two binaries allow, and finds one premise of §0 wrong: the point
`SELECT`'s 17.5 µs over `SHOW META` is mostly the client's (on A, +2.2 µs of
server CPU and +12.5 µs of driver CPU), not the engine's.

**Docs moved to their owners**: `heap-and-tuple.md` §3.4 (carried values, the
release rule, the tail hint), `rule-fixed-length-tuple.md`, `types.md` §3.2b
(what an UPDATE decodes and encodes), `protocol.md` §5 and `client-manual.md`
(one write per pipelined statement), `known-gaps.md`'s BK section, `CLAUDE.md`'s
var-heap and wire rows, `index.md`'s row. The manual's SQL surface did not
change. **Synced**: `origin/main` merged at `8679367e` (one conflict, `index.md`,
three rows kept side by side - this order's, `workorder-bk-sys-statistics-show-sugar.md`'s
and BL's), the full Debug suite green after it (3521/3521).

### BK-S2, BK-S6, BK-S7 — not built, 2026-10-11

- **BK-S2** (compile once, BK-R1): parse and compile are 1.1-4.5 µs of a point
  statement's server CPU at the close (5-15 %), reachable only by a client
  that prepares; BK-Q13. Recorded in `known-gaps.md`.
- **BK-S6** (access statistics per core, BK-R6): `Catalog::RecordAccess` is
  0.7 % of a point `SELECT` (BK-S1's census).
- **BK-S7** (byte-range records, BK-R5): the every-row UPDATE's undo and WAL
  cost is per record - the undo append 28 % of a row, the WAL append 7.5 % -
  for a 44-byte image (`profB1r-reports.txt`); BK-Q6's condition is not met.
