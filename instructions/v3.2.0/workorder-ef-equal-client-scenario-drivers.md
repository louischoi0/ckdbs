# Work order EF - the scenario drivers on an equal client, and a short read's fixed cost only if one is left behind it

Status: **CLOSED 2026-11-28 at EF-S2, the engine stage (S3) not opened** (EF-Q1 (a) read on the matched run: neither `pk` nor the 200-row tape is behind PostgreSQL through the twins' client modes; §7, "EF-S4"). Not landed: the work branch waits for the operator's word. **OPENED 2026-11-28** on worktree
`ef-equal-client-scenario-drivers` (branch
`worktree-ef-equal-client-scenario-drivers`) from `72580aa04`
(`v3.5.1-6-g72580aa04`), under the operator's standing go-ahead of
2026-11-28 (`raft-marks-2026-11-28.md` §7): every EF-Q item is **adopted
on that go-ahead, never marked by the operator item by item** (§4,
"Marks"). Written on worktree
`pg-kds-comparison-orders` (branch `worktree-pg-kds-comparison-orders`)
from `b95be2e86` (`v3.4.17-255-gb95be2e86`) on the operator's word of
2026-11-28 (`raft-marks-2026-11-28.md` §4, §5). No stage is started; EF-S0
waited for the operator's word and for the marks on §4's items. A
`[source-read]` claim with a `path:line` was read at `b95be2e86`. **Two
readers stand behind them, and the order says which**: the driver and
client lines of §1.1 and the files under `bench/` were read by CLA; the
server path of §1.3 was read by a research pass of this session and its
lines were not re-read by CLA.

Tags: `[operator]`, `[source-read]`, `[measured]`, `[design]`,
`[assumed]`, `[quiet-wrong]`, as in DT's order.

Related: `bench/v3.5.0/comparison-pg-kds-scenarios-v3.4.17-166-g6c98f7c32.md`
(the reading EF answers);
`bench/v3.5.0/results-pg-control-scenarios-v3.4.17-202-g481d101aa.md` (the
PostgreSQL side); `bench/v3.2.0/results-pg-parity-v3.2.0-171-ge81ac7cd.md`
(the client difference, measured once); `tools/kwp.py`, `tools/ckdbs_cli.py`,
`tools/pg_wire.py`, `tools/parity_bench.py`; `bench/README.md`;
`instructions/v3.2.0/workorder-ea-prepared-statement-reuse.md` (EA, closed
2026-11-27); `instructions/v3.2.0/workorder-cx-managed-plans.md` CX-S6;
EG and EH, written the same day.

## 0. What EF is

`[measured]` Through the scenario drivers, on one host with both servers
pinned, a short read is slower on KDS than on PostgreSQL 18.7: scenario
4's `pk` control reads 31.9 µs against 23.2 µs, its 200-row tape 45.3 µs
against 33.4 µs, and a one-booker booking's four reads are 7.6 µs to
28.5 µs dearer each (the comparison file, "What the matrix says", items 1
and 3). The KDS side is `6c98f7c32` and `9f8f8b354`, not `main`.

`[source-read]` **The two sides are not timed through equal clients**
(§1.1), and the one time the difference was measured it was larger than
the gap: 29.0 µs against 5.0 µs of client CPU per point statement, with
the lookup at 1.30x of PostgreSQL's rate once the clients were equal
(§1.2). So the premise "the engine's fixed cost of a short read is
higher" is not supported, and is not refuted on this host either.

`[design]` EF does three things in order, and stops where the evidence
does: **the scenario drivers time a client that does what the twin's
does** (S1); **the matched cells are measured again on `main` beside
PostgreSQL in one window** (S2), which replaces the comparison file; and
**only if a short read is still behind** does EF attribute and cut the
server's per-statement work (S3, behind EF-Q1's gate).

## 1. The tree as read

### 1.1 The clients `[source-read]`, by CLA

- **Every KDS scenario driver times `send_command`**:
  `tools/scenario1_backtest.py:400`, `tools/scenario2_freight.py:298`,
  `tools/scenario3_library.py:232`,
  `tools/scenario4_cabinopt_days.py:250`, each
  `format_reply(conn.send_command(...))`.
- **`send_command` decodes and renders** (`tools/ckdbs_cli.py:191-228`):
  it calls `kwp.Connection.execute`, which builds a list per row and a
  bytes object per field, then renders every field to text and joins the
  rows into one string.
- **Only scenario 4's twin times a client that counts** (corrected at
  EF-S0; the order as written said "scenarios 2, 3 and 4"): its `timed`
  goes through `PgConnection.send_command`, which is `query` - rows
  counted, column lengths walked, no value decoded
  (`tools/pg_scenario4_cabinopt_days.py:56-70`, `tools/pg_wire.py:224-262`,
  `:313-322`). `_skip_data_row` says of the KDS side that `send_command`
  "is not one" - not its counterpart (`tools/pg_wire.py:256-262`).
- **Scenario 2's and 3's twins decode their reads**: `rows_timed` and
  `Client.timed` call `PgConnection.fetch` and decode every value to a
  string, with no rendering (`tools/pg_scenario2_freight.py:140-145`,
  `:164-168`, `tools/pg_scenario3_library.py:108-121`). **So does every
  `INSERT ... RETURNING id`** through their `insert_returning`
  (`tools/pg_scenario2_freight.py:196-200`,
  `tools/pg_scenario3_library.py:186-190`): scenario 2's `freight-insert`
  and its six load phases, scenario 3's users and books. Their other
  writes (`charge-insert`, `operation-update`, `org-update`, `commit`,
  scenario 3's reservations and loans), their DDL and `ANALYZE` go
  through `query`, which counts. **Scenario 1's twin decodes and
  renders** every statement it times: its `PgClient` joins every field
  into the KDS client's reply shape, "the client pays the same price on
  both sides" (`tools/pg_scenario1_backtest.py:78-83`, `:266-299`); its
  concurrency sweep alone calls `fetch` and decodes nothing
  (`:741-742`). So the clients are unequal in scenarios 2 to 4 by a
  different amount in each, and EF-S0 measured those pairs (§7);
  scenario 1's pair is not measured.
- **The counterpart exists and no scenario driver calls it**:
  `kwp.Connection.query` (`tools/kwp.py:654`) returns
  `(tag, rows, payload_bytes, error)` as `pg_wire`'s does, and its
  docstring prices what `execute` and `send_command` add: "about 3.8 µs
  per returned row and 25 µs per statement on this Python" at 9 int64
  fields.
- **A KDS statement is one round trip of five frames** (PARSE, BIND,
  EXECUTE, CLOSE, SYNC in one write, an unnamed statement); PostgreSQL's
  is one simple-query message. `[source-read]` by the research pass
  (`tools/kwp.py:560-652`).
- `[assumed]` **Some timed phases need their values**: the backtest
  decides from the prices it reads and a booking prices from the rows it
  reads, on both engines. Which phases those are, and what each twin
  decodes for them, is not read here; S0 lists them (EF-Q2).

### 1.2 What was measured `[measured]`, each at its commit

- **`e81ac7cd`, another host**
  (`bench/v3.2.0/results-pg-parity-v3.2.0-171-ge81ac7cd.md:3-24`, `:68-80`):
  client CPU 29.0 µs against 5.0 µs per point statement and 6.3 µs
  against 2.5 µs per returned 9-column row. On equal clients: `pk-lookup`
  1.30x of PostgreSQL's rate on 0.64x its server CPU, `bar-range` 1.42x,
  `symbol-history` 1.26x; `agg-global` 0.61x on 1.63x the server CPU,
  which is EH's.
- **`481d101aa` to `2c5c195f7`, this host**
  (`bench/v3.4.17/results-ea-close-v3.4.17-205-g2c5c195f7.md:56`): a text
  point `SELECT` is 6.415 µs of server CPU before EA and 6.460 µs after.
- **`9712f5f0`** (`bench/v3.4.12/results-cx-s6-compile-share-v3.4.12-112-g9712f5f0.md`,
  as the research pass read it): a point read's client p50 of 40.2 µs was
  about 33 µs of client and socket and 7.2 µs of server.
- **This host, 2026-11-28** (the PostgreSQL control): `pk` 23.2 µs
  (1,000,000 / its median of 43,103 qps, not a mean), through the client
  that counts.
- **Not measured anywhere**: the client difference on this host and this
  Python; a short read on `main` through equal clients; PostgreSQL's
  server CPU for a point statement on this host.

### 1.3 The server's work for one autocommit point `SELECT` `[source-read]`, by the research pass, none of it timed

In order, with what each does per statement:

- **Decode**: one `read`; each of the five frames is copied into a new
  vector and erased from the front of the buffer
  (`src/wire/frame_codec.cpp:85-114`).
- **PARSE and BIND**: the text is scanned for placeholders and lexed and
  hashed whole for its fingerprint, then scanned and copied again by the
  substitution; the portal is inserted into a map under the name the
  client sent, `p%d` from `execute` (`tools/kwp.py:568`)
  (`src/server/kwp_session.cpp:646-690`, `:692-740`, the insert at
  `:735`). **The text is then
  lexed and parsed a second time** in the dispatcher
  (`docs/inflight/known-gaps.md:1733`).
- **Dispatch**: a coroutine frame is a heap allocation
  (`include/kds/sched/coro.hpp:26-30`), about six a statement (the count is
  the research pass's and unverified: the cited lines state the allocation,
  not the count); the statement epoch is
  entered and published and the schema word read.
- **The view and the lease**: `MintReadView` calls `SnapshotCeiling`
  twice, each a loop over every core's slot
  (`src/txn/manager.cpp:91-126`, `src/txn/instance_visibility.cpp:138-158`),
  and the reader lease publishes the core's bounds at take and at release
  (`src/txn/manager.cpp:649-689`).
- **The borrow**: the relation `IS` through the lock table, taken and
  released (`include/kds/server/read_borrow.hpp:94-115`).
- **Compile**: a one-relation block skips the plan lookup and the
  join-order search; `InitTableAccess` is a hash lookup, run at the
  compile's bind (`src/exec/step_compiler.cpp:3051`) and again at the
  executor's (`src/exec/step_vm.cpp:309`) - the research pass's "three
  times" is unverified. **No compiled chain is kept**
  (CX-Q37 (a), EA-Q3 (a)): `exec::Compile` is 8.1 % to 8.2 % of a
  prepared point `SELECT`'s server CPU with its parse removed, at
  `481d101aa`
  (`bench/v3.4.17/results-ea-s0-premise-v3.4.17-202-g481d101aa.md:10-11`).
- **The descent**: `StillOwned` reads two pages, the anchor and the
  descriptor, on every statement before the lookup
  (`src/exec/step_vm.cpp:305-338`).
- **The reply**: each row is encoded into a vector and copied into a
  string, then into the open batch and again at its seal
  (`src/server/kwp_session.cpp:287-366`); each frame is encoded into a
  vector and copied into the connection's frames (`:429-433`), copied
  again into the outbox (`src/server/tcp_server.cpp:633-643`, `:1203-1215`),
  and leaves in one `send` (`:884-905`). BN's close put the floor of a
  `C_PING` at 12.2 µs of CPU with 64 % of it in the `send`, at `8679367e`
  (`bench/v3.0.0/results-bn-close-v2.7.0-787-g8679367e.md:160`), an engine
  and a host other than this order's.

Against a client-observed 23 µs to 32 µs, the server's whole 6.4 µs is
the smaller part; that is what the tree says about how much S3 could
move.

## 2. The mechanism `[design]`

- **S1, the drivers.** A timed call names what it consumes: `count`
  (rows and bytes, `kwp.Connection.query` beside `pg_wire`'s `query`) or
  `values` (decoded to the same depth on both sides, no text rendering on
  either). Verification and the byte comparisons keep `send_command`,
  outside the timer. Nothing in the engine changes.
- **S2, the matched run.** `main`, Release, a copy of the binary; the
  cells the PostgreSQL results file's section 2 names; KDS and
  PostgreSQL interleaved by round in one window, both servers on CPUs 0
  to 7 and both drivers on 8 to 23. Client CPU and server CPU are
  recorded beside the wall clock for the short reads.
- **S3, the server, gated.** A census of §1.3 in server CPU, then cuts
  ranked by it, each measured interleaved. Nothing is designed before the
  census.

## 3. Rules

- **EF-R1. A timed phase does equal client work on both sides, and the
  results file says which mode each phase ran in.**
- **EF-R2. The driver's old numbers and its new ones are two series.** No
  delta is taken across S1 except on the one overlap cell S1 runs through
  both clients to state the break.
- **EF-R3. Verification does not weaken**: every `--verify`, every byte
  comparison and every `--assert-index-reads` still runs and still reads
  values.
- **EF-R4. S1 and S2 change no engine source.**
- **EF-R5. An S3 cut changes no result, no frame and no refusal**, and is
  measured in server CPU, interleaved A/B, in `build-release`
  (`CLAUDE.md`, Working Rules).
- **EF-R6. This host is pinned** (server on the P-cores), and the results
  file says so.
- **EF-R7. Two of §1.3's costs are checks, and S3 cuts neither.**
  `StillOwned`'s two page reads are what refuses a statement whose
  relation is no longer there (`src/exec/step_vm.cpp:318-338`), and
  `MintReadView`'s first ceiling read is its announce before it takes
  (`src/txn/manager.cpp:112-124`). A statement that skips either can
  answer from a relation or a view it does not hold: `[quiet-wrong]`.
  S3 may make one cheaper only with the check it performs shown intact.
- The standing rules, as DT's order lists them.

## 4. Items for the operator (EF-Q), CLA's proposals

| | item | options | CLA proposes |
|---|---|---|---|
| EF-Q0 | Is EF opened, and with what scope? | (a) S0 to S2, S3 behind EF-Q1; (b) the drivers and the run only, no engine stage ever; (c) the engine census at once, on the old drivers | (a) |
| EF-Q1 | When does the engine stage open? | (a) only if, through equal clients on `main`, `pk` or the 200-row tape is behind PostgreSQL by more than the larger spread; otherwise EF closes at S2 with the server path of §1.3 left as read. (b) S3 runs regardless | (a) |
| EF-Q2 | A phase whose values the driver needs | (a) both sides decode to one depth through one small decoder each, no rendering; (b) such phases stay unequal and are labelled, and only `count` phases are compared | (a) |
| EF-Q3 | The series break | (a) the new client is the drivers' only timed path, the old numbers are not compared, and one overlap cell states the break; (b) the old path kept behind a flag as the default | (a) |
| EF-Q4 | The PostgreSQL side of S2 | (a) run again in S2's window, interleaved by round with KDS; (b) the control run of 2026-11-28 reused | (a) |
| EF-Q5 | S2's time | (a) the 20-minute stage budget lifted for S2 alone (estimate: 45 to 60 minutes, basis: the `v3.5.0` run's 44 minutes plus PostgreSQL's 14, confidence: medium); (b) cut to fit: one round a cell, scenario 4 and the size sweeps left out | (a) |
| EF-Q6 | The compiled chain | (a) stays where CX-Q37 (a) and EA-Q3 (a) left it: S3 may measure its share again and may not build its reuse without the operator's word; (b) reopened inside S3 | (a) |
| EF-Q7 | Scenario 1, whose twin already decodes as the KDS client does (§1.1) | (a) left on its equal-decode pair, named a `values` phase throughout, and S1 moves scenarios 2 to 4 only; (b) both of its sides moved to `count`, a second series break | (a) |
| EF-Q8 | (written at EF-S2) Scenario 1's point reads are behind through its own pair - `read-bar-lookup` 0.62x to 0.76x in EF-S2's run a - because the KDS client builds four frames and decodes the row description on every statement, which the twin's `fetch` has no equivalent of; EF-Q7's "already equal" was wrong for a statement | (a) left and labelled; (b) scenario 1's KDS statements through `ServerConnection.rendered` - the reply string `send_command` returns, byte for byte, over the request and the cached description `fetch` uses - and its cells measured again; (c) `send_command` itself rebuilt that way, which moves every series under `bench/` at once | (b) |

### Marks

**Every row is CLA's proposal adopted on the operator's standing go-ahead
of 2026-11-28 (go-ahead-achieving-milestone)** - *"go ahead until
achieving milestone, I will follow CLA proposal if decision needed"*,
given as "proceed with the milestones in order, as proposed"
(`raft-marks-2026-11-28.md` §7) - and none is the operator's own mark on
the item. No EF-Q item is tagged `[quiet-wrong]`; EF-R7, which is, is a
rule and binds S3 only.

| | adopted | note |
|---|---|---|
| EF-Q0 | (a) | S0 to S2, S3 behind EF-Q1 |
| EF-Q1 | (a) | the engine stage opens only on a short read behind PostgreSQL by more than the larger spread, on equal clients on `main` |
| EF-Q2 | (a) | a phase whose values the driver needs decodes to one depth on both sides, no rendering |
| EF-Q3 | (a) | the new client is the only timed path; one overlap cell states the break |
| EF-Q4 | (a) | PostgreSQL run again in S2's window, interleaved by round |
| EF-Q5 | (a) | **the 20-minute stage budget is lifted for S2 by this adoption, not by the operator's own word for that measurement**; S2 is still planned as short as its cells allow and its file states the time |
| EF-Q6 | (a) | the compiled chain stays where CX-Q37 (a) and EA-Q3 (a) left it |
| EF-Q7 | (a) | scenario 1 stays on its equal-decode pair; S1 moves scenarios 2 to 4 |
| EF-Q8 | (b) | written at EF-S2 and adopted on the same go-ahead; **it revises EF-Q7 (a) for scenario 1's KDS side**: the twin is untouched, the KDS reply string is unchanged, and the cells were measured again (run c) |

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **EF-S0** | The premise on this host: `pk`, the 200-row tape and a booking's reads on `main` through both clients, client CPU apart from server CPU; the list of phases that need values | the break stated in µs per statement and per row; EF-Q2's list |
| **EF-S1** | The four drivers on the equal client; the overlap cell | every verification passing; the drivers' new series opened |
| **EF-S2** | The matched run of EF-Q4 and EF-Q5; the comparison file replaced by a results file | EF-Q1 read: closed here, or S3 opened |
| **EF-S3** | (gated) The census of §1.3 in server CPU, then its cuts | each cut's measured delta; the suite |
| **EF-S4** | The close: `bench/README.md`'s rule on equal clients, `known-gaps.md`, the overhead measurement if S3 ran | the operator's word on landing |

## 6. What EF does not do

- No change to KWP, to a frame or to what a statement answers.
- No reuse of a compiled chain (EF-Q6).
- No tuning of PostgreSQL and no change to a twin's SQL.
- Nothing of the commit (EG) or of the fold (EH).

## 7. Stage log (CLA, appended as stages land)

### EF-S4 (2026-11-28): the close

- **EF closes at S2 with S3 not opened** (EF-Q1 (a)). Built: the
  drivers of scenarios 1 to 4 on their twins' client modes
  (`tools/kwp.py` `fetch`, `tools/ckdbs_cli.py` `rows`, `count`,
  `rendered`), `tools/parity_bench.py`'s client choices and its host pid
  for a containerised backend, and three results files
  (`bench/v3.5.1/results-ef-s0-premise-...`, `-ef-s1-drivers-...`,
  `-ef-s2-matched-...`). Not built: anything in the engine.
- **The durable content**: `bench/README.md`, "A ratio against another
  engine names both clients"; the comparison file carries its
  supersession; §1.3 of this order stays as read, a list of what a
  statement costs and not of a deficit.
- **The milestone-close A/B was not taken, and there is nothing for it
  to compare**: `git diff 72580aa04 HEAD -- src include tests
  CMakeLists.txt scripts` is empty, so the opening and closing commits
  build one binary. The suite was not executed for the same reason: no
  cell of it reads `tools/`.
- **What EF hands on**: to EG, `result-insert` at 60,480 bars (0.81x at
  `strict`, no claim at the three smaller sizes, a spread over 20 % on
  one side of each); to EH, the folds
  (`agg-global` 0.71x on 1.40x the server CPU through counting clients,
  `agg-distinct` 0.73x to 0.83x, `close` 0.70x); to nobody, the per-row
  render of a typed value in the Python client.

### EF-S2 (2026-11-28): the matched run, on `ef-equal-client-scenario-drivers` from `3321c61fb`

`[measured]` at engine `v3.5.1-6-g72580aa04`,
`bench/v3.5.1/results-ef-s2-matched-v3.5.1-6-g72580aa04.md`, three
rounds, KDS and PostgreSQL 18.7 interleaved, 151 cells, every return code
0 and every verification passing:

- **EF-Q1 read: S3 does not open.** Through its twin's client mode
  neither read the gate names is behind PostgreSQL: scenario 4's `pk`
  20.5 µs against 21.2 µs (inside the spread, KDS the faster in each
  round), and **the 200-row tape carries no claim** - PostgreSQL's three
  runs spread 49.4 % - with KDS the faster in every round (26.2 µs to
  27.1 µs against 32.8 µs to 51.9 µs). Beside them, a booking's point
  reads are 21.8 µs to 26.1 µs against 21.9 µs to 25.4 µs and scenario
  1's lookup 24.9 µs to 27.2 µs against 28.8 µs; **two short reads are
  behind by more than the larger spread, by under 1 µs**: `cargo-lookup`
  at `strict` (0.7 µs on a 2.0 % spread, inside it at `group`) and
  `books-by-genre` over 200 unindexed loans (0.8 µs on 3.2 %), each less
  than the 2.9 µs of client CPU EF-R1 leaves unequal on a decoded read.
  A gate read on every short read would have been met.
- **One booker**: 2,077.5 bookings a second against 2,070.2; every
  write and the commit ahead (1.05x to 1.38x at `strict`).
- **Scenario 3**: a geometric mean of 1.09x to 1.42x over its six cells.
- **Behind PostgreSQL on these clients**: folds over a whole relation,
  `result-insert` at 60,480 bars, and a read returning thousands of rows
  through the rendering pair.
- **EF-Q8 written and adopted (b)** in this stage: run a read scenario
  1's lookup at 0.62x to 0.76x, which refuted EF-Q7's premise;
  `ServerConnection.rendered` answers `send_command`'s string byte for
  byte (16 deterministic replies compared on a live server; an `ANALYZE`
  differs only in the timing text two `send_command` runs differ in),
  and run c read the lookup at 0.99x to 1.19x.
- **Three runs, not one**: run a (every cell, 23 minutes), run b
  (scenarios 2 and 3 again after EF-S1's write change, and one scenario
  4 cell CLA's own smoke test had run inside), run c (scenario 1 again
  after EF-Q8). About 36 minutes, over the stage budget EF-Q5 (a) lifted
  by adoption.
- The suite was not executed: no build input changed (`tools/` only).

### EF-S1 (2026-11-28): the drivers, on `ef-equal-client-scenario-drivers` from `5df8ffe05`

`[measured]` at engine `v3.5.1-6-g72580aa04`,
`bench/v3.5.1/results-ef-s1-drivers-v3.5.1-6-g72580aa04.md`:

- **Each driver's timed path is now its twin's** (EF-Q2 (a), EF-Q7 (a)):
  scenario 2's seven reads and scenario 3's read shapes decode each
  value to a string and render nothing (`rows_timed`, `Client.timed`);
  scenario 4's timed statements are counted (`Arm.timed`); scenario 1 is
  untouched. Verification and the byte comparisons still read through
  `send_command`, outside the timer (EF-R3).
- **`kwp.Connection.fetch`** is the values path: the request `query`
  sends, and a row description decoded once per shape, keyed by its
  bytes. **CLA's first form of it did not cache the description and was
  measured still behind** (a lookup at 32.2 µs against PostgreSQL's
  23.4 µs, pass D): a KDS client must read the description to render
  typed values, a PostgreSQL client receives text and reads none, and in
  Python that step cost about 10 µs a statement. Cached, the lookup is
  22.8 µs against 22.8 µs (pass D2) **on the wall clock only**: the KDS
  client spends 11.5 µs of CPU a statement against PostgreSQL's 8.6 µs
  (spreads 4.3 % and 3.0 %), and its server 7.6 µs against 10.6 µs.
- **The series break** (EF-Q3 (a), the overlap, three interleaved rounds
  on the final driver): a booking's four reads are 7.4 µs to 11.7 µs
  cheaper each, three of its writes and its commit inside their spreads
  and `operation-update` and `org-update` 0.6 µs and 0.9 µs dearer through
  write code the two drivers share (not attributed), the booking rate
  1,978.6 against 1,829.8 a second; ten of scenario 3's twelve read
  shapes are 10 µs to 47 µs cheaper. The outcomes and `--verify` are
  identical through both drivers.
- **What stays unequal**: per row, the wire's - a typed value is
  rendered by the KDS client, a text value is not by PostgreSQL's
  (`bar-range` 0.96x through the values pair, 1.74x through two counting
  clients) - and per statement. **EF-R1's "equal client work" is not met**: a `values`
  phase still costs the KDS client about 2.9 µs of CPU a statement more
  than its twin's (not attributed) and more again per row, and the
  writes are in another mode (next item); the results files say which mode
  each phase ran in, from the `timed_client` entry the drivers now write.
- **Not moved, and not equal**: writes keep `send_command`, which reaches
  `kwp.Connection.execute`, where the twins' writes go through
  `pg_wire`'s `query` (counted) or, for `RETURNING`, `fetch` (one value
  decoded). The overlap cannot show the difference, since both KDS
  drivers time the same write code; it is not measured on this host
  (a mock socket, no server, puts `send_command` of a write at 3.8 µs of
  Python against `count`'s 1.9 µs). **Not run**: an overlap for scenario 4, whose
  change is the counting client EF-S0's pass A measured, and **any
  scenario 4 run at all on the new driver**, so its byte comparisons have
  not yet passed through it (the stage's "every verification passing" is
  met for scenarios 2 and 3 only). Run 1's new driver copy (`fetch`
  before its cache) was not hashed.
- The suite was not executed: no build input changed (`tools/` only).
  Overhead not measured; nothing in the engine changed.

- **After the review (`189c901fa`): timed writes moved to the counting
  client.** The review showed EF-R1 unmet for writes: KDS timed them
  through `send_command` and the twins through `query`, about 2 µs of
  Python a statement by the review's mock (3.8 µs against 1.9 µs, no
  server, off the bench host). `send` in scenarios 2 and 3 now calls
  `Client.count`, which returns the tag `send_command` returns, so an
  issued id is read as before; the outcomes of a 200-booking run are
  unchanged. **Not measured in this stage**: EF-S2's scenario 2 and 3
  cells are run on this form. What still differs: the twins' `RETURNING`
  inserts decode one value where KDS reads its tag (against PostgreSQL,
  under 1 µs a statement by EF-S0's `fetch` figure), and a `values`
  phase's client CPU (11.5 µs against 8.6 µs on the lookup, not
  attributed).

### EF-S0 (2026-11-28): the premise, on `ef-equal-client-scenario-drivers` from `72580aa04`

`[measured]` at `v3.5.1-6-g72580aa04`, Release, both servers on CPUs 0 to
7, `bench/v3.5.1/results-ef-s0-premise-v3.5.1-6-g72580aa04.md`:

- **Through clients that count on both sides a lookup is 20.1 µs on KDS
  against 22.1 µs on PostgreSQL 18.7** (1.10x, spreads 7.7 % and 6.7 %),
  on 7.0 µs against 10.9 µs of server CPU; the four other reads that
  fold nothing are 1.47x to 2.01x and the grouped fold 1.20x.
- **The break**: the KDS scenario client (`send_command`) adds 16.6 µs a
  statement and 1.4 µs to 1.7 µs a returned 8-column row over the
  counting one; PostgreSQL's decoding client (`fetch`) adds 0.8 µs and
  0.7 µs to 0.8 µs. Through scenario 4's pair the same lookup reads 0.62x
  and through scenario 2's and 3's 0.63x.
- **One shape is behind on equal clients, and it is EH's**: `agg-global`
  at 0.71x on 1.40x the server CPU, 39.5 ns against 28.1 ns a row.
- **EF-Q1, read early**: on this evidence no short read is behind
  PostgreSQL on equal clients, so S3 does not open. The evidence is
  `pk`'s alone - the 200-row tape the gate also names was not run - and
  the gate is read again at S2, on the scenario cells themselves.
- **EF-Q2's list** (`[source-read]` at `72580aa04`): scenario 2's four
  booking reads and its verification reads need their values on both
  sides, and `freight-insert` its returned id; its three manifest reads
  need none (`manifest-scan` its row count); scenario 3's twelve read shapes are timed through a decode on
  PostgreSQL and need no value on either side; scenario 4's phases need
  none; scenario 1 is EF-Q7's.
- **Cut**: a booking's reads were not run in S0 (they are point reads
  through the same two clients; the scenario 2 schema is S2's), nor was
  the 200-row tape (no shape of the driver is it), and no quiet gate was
  applied to a 30-second run.
- **The tool**: `tools/parity_bench.py` takes `--kds-client` and
  `--pg-client` and reads a containerised backend's `schedstat` by its
  host pid.
- The suite was not executed: no build input changed. Overhead not
  measured; nothing in the engine changed.

### Written 2026-11-28

On `pg-kds-comparison-orders` from `b95be2e86`, on the operator's word.
No stage is opened and no code is written. The premise EF was asked for -
"the fixed cost of a short statement is PostgreSQL's lead" - was CLA's
reading of the comparison's first version and is corrected in that file;
the operator chose this order's re-scoped form over the original
(`raft-marks-2026-11-28.md` §5).
