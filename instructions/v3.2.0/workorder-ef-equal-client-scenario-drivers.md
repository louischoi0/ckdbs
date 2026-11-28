# Work order EF - the scenario drivers on an equal client, and a short read's fixed cost only if one is left behind it

Status: **WRITTEN 2026-11-28, not opened.** Written on worktree
`pg-kds-comparison-orders` (branch `worktree-pg-kds-comparison-orders`)
from `b95be2e86` (`v3.4.17-255-gb95be2e86`) on the operator's word of
2026-11-28 (`raft-marks-2026-11-28.md` §3, §4). No stage is started; EF-S0
waits for the operator's word and for the marks on §4's items. A
`[source-read]` claim with a `path:line` was read at `b95be2e86`. **Two
readers stand behind them, and the order says which**: the driver and
client lines of §1.1 and the files under `bench/` were read by CLA; the
server path of §1.3 was read by a research pass of this session and its
lines were not re-read by CLA.

Tags: `[operator]`, `[source-read]`, `[measured]`, `[design]`,
`[assumed]`, as in DT's order.

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
- **Every PostgreSQL twin times `PgConnection.query`**
  (`tools/pg_wire.py:224-262`): rows are counted and their column lengths
  walked, and no value is decoded. Its `_skip_data_row` says of the KDS
  side that `send_command` "is not one" - not its counterpart
  (`tools/pg_wire.py:256-262`).
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
- **This host, 2026-11-28** (the PostgreSQL control): `pk` 23.2 µs mean,
  through the client that counts.
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
  substitution; a portal named `p%d` is inserted into a map
  (`src/server/kwp_session.cpp:287-366`, `:429-433`). **The text is then
  lexed and parsed a second time** in the dispatcher
  (`docs/inflight/known-gaps.md:1733`).
- **Dispatch**: a coroutine frame is a heap allocation, about six a
  statement (`include/kds/sched/coro.hpp:26-30`); the statement epoch is
  entered and published and the schema word read.
- **The view and the lease**: `MintReadView` calls `SnapshotCeiling`
  twice, each a loop over every core's slots, and the reader lease
  publishes the core's bounds at take and at release
  (`src/txn/manager.cpp:649-689`, `src/txn/instance_visibility.cpp:138-158`).
- **The borrow**: the relation `IS` through the lock table, taken and
  released (`include/kds/server/read_borrow.hpp:94-115`).
- **Compile**: a one-relation block skips the plan lookup and the
  join-order search; `InitTableAccess` is a hash lookup run three times
  (`src/exec/step_compiler.cpp:3004-3089`). **No compiled chain is kept**
  (CX-Q37 (a), EA-Q3 (a)): `exec::Compile` is 8.1 % to 8.2 % of a
  prepared point `SELECT`'s server CPU.
- **The descent**: `StillOwned` reads two pages, the anchor and the
  descriptor, on every statement before the lookup
  (`src/exec/step_vm.cpp:305-338`).
- **The reply**: each frame is encoded into a vector, copied into the
  session's frames and again into the outbox, and leaves in one `send`
  (`src/server/tcp_server.cpp:740-815`). BN's close put the floor of a
  `C_PING` at 12 µs of CPU with 64 % of it in the `send`.

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

### Written 2026-11-28

On `pg-kds-comparison-orders` from `b95be2e86`, on the operator's word.
No stage is opened and no code is written. The premise EF was asked for -
"the fixed cost of a short statement is PostgreSQL's lead" - was CLA's
reading of the comparison's first version and is corrected in that file;
the operator chose this order's re-scoped form over the original
(`raft-marks-2026-11-28.md` §4).
