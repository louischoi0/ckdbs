# Work order EH - what the native fold pays for a row: an aggregate over a whole relation

Status: **WRITTEN 2026-11-28, not opened.** Written on worktree
`pg-kds-comparison-orders` (branch `worktree-pg-kds-comparison-orders`)
from `b95be2e86` (`v3.4.17-255-gb95be2e86`) on the operator's word of
2026-11-28 (`raft-marks-2026-11-28.md` §3, §4). No stage is started; EH-S0
waits for the operator's word and for the marks on §4's items. A
`[source-read]` claim with a `path:line` was read at `b95be2e86`. **Two
readers stand behind them**: the lines marked "spot-read" were re-read by
CLA; the rest was read by a research pass of this session and not
re-read.

Tags: `[operator]`, `[source-read]`, `[measured]`, `[design]`,
`[quiet-wrong]`, as in DT's order.

Related: `docs/spec/aggregate.md` (AG1-AG15); `docs/spec/parser-v2.md`
§7, I19; `docs/spec/sched.md` §3; `docs/spec/index.md` IX7;
`bench/v3.2.0/results-pg-parity-v3.2.0-171-ge81ac7cd.md` (the fold's cost
on equal clients);
`bench/v3.5.0/comparison-pg-kds-scenarios-v3.4.17-166-g6c98f7c32.md`;
`bench/v3.5.0/results-pg-control-scenarios-v3.4.17-202-g481d101aa.md`;
`git show 7f0193bac^:docs/inflight/in-progress/workplan-aggregate-perf.md`
(the closed plan whose AP05 was not built); EF and EG, written the same
day.

## 0. What EH is

`[measured]` Through the scenario drivers, scenario 4's `close` -
`COUNT`, `SUM` and a `GROUP BY` over whole relations - reads 1,011.4 µs
on KDS at `6c98f7c32` against 474.4 µs on PostgreSQL 18.7, 0.47x, the
largest gap in the comparison. The drivers' unequal clients are in that
figure (one of the four statements returns 1,000 rows; EF §1.1), so it
does not size the engine's part.

`[measured]` **The engine's part was sized once, on equal clients**: at
`e81ac7cd`, `agg-global` over 60,480 rows ran at 0.61x of PostgreSQL's
rate on 1.63x its server CPU - 5,776 µs against 3,549 µs, about 95 ns
against 59 ns per row folded - and `agg-by-session` used 1.21x
PostgreSQL's server CPU while leading on the wall clock
(`bench/v3.2.0/results-pg-parity-v3.2.0-171-ge81ac7cd.md:14-22`,
`:68-80`). Of the three leads the comparison gave PostgreSQL, this is the
one the tree supports as the engine's. Nothing since is known to have
moved it, and it is not re-measured on `main`.

`[design]` EH lowers what the native fold pays for a row, **without
changing a result, a refusal or the order groups are emitted in**. S0
measures the premise on `main` and ranks §1.2's costs before anything is
built.

## 1. The tree as read

### 1.1 The path `[source-read]`

- **All four `close` statements are native.** `SELECT COUNT(*) FROM
  board`, `SELECT SUM(qty) FROM board`, `SELECT symbol, COUNT(*) FROM
  board GROUP BY symbol` and `SELECT COUNT(*) FROM tape_10k`
  (`tools/scenario4_cabinopt_days.py:434-443`), three rounds, 12
  operations a phase; the board holds 10,240 rows and 1,000 symbols, the
  tape 10,000 rows. None reaches the query layer, which folds what the
  native parse or compile declines (`HAVING`, an order over aggregates,
  expressions; `aggregate.md` §2).
- **The fold is outside the executor** (AG1, `aggregate.md:17`,
  spot-read): the dispatcher wraps the statement's `RowSink` in an
  `Aggregator` and the compiled chain is byte-identical to the same
  statement without the fold.
- **It runs whole, without a slice.** `RunAggregated` calls the
  synchronous `exec::Execute` with a lambda that calls
  `Aggregator::Accumulate`
  (`src/server/command_dispatcher.cpp:11037-11043`, spot-read). A plain
  `SELECT`'s outermost walk is a `SelectRun` that yields every 64 pages
  (`sched.md` §3); an aggregated one holds its core from its first leaf
  to its last. `sched.md` does not name the aggregate either way
  (spot-read: no line of it mentions one).

### 1.2 What a row costs on that path `[source-read]`, none of it timed

- **Visibility**: `txn::Classify` per row (`src/exec/step_vm.cpp:2970`).
- **A copy of the payload**, whatever the fold reads: the visible row's
  bytes are assigned into a buffer before any decode
  (`src/exec/step_vm.cpp:3005-3006`, spot-read), for `COUNT(*)` as for
  anything else.
- **No scan kernel**: the kernel is compiled from the `WHERE` residual
  and is null without one (`src/exec/step_vm.cpp:2956-2962`, spot-read;
  `:3358-3385`).
- **The decode** of what the statement reads: nothing for `COUNT(*)`,
  the `int64` for `SUM(qty)`, and for the `GROUP BY` the `varchar` key
  copied into a string (`src/exec/row_codec.cpp:559-566`).
- **A `std::function` call per row**: `RowSink`
  (`include/kds/exec/step_vm.hpp:58`, spot-read).
- **The accumulation** (`src/exec/aggregate.cpp:337-379`): without
  `GROUP BY`, one group and no hashing; with it, the key encoded into a
  reused scratch buffer and probed in a `std::unordered_map` keyed by
  `std::string` under `std::hash<std::string_view>`
  (`include/kds/exec/aggregate.hpp:145-150`, spot-read; `:243-244`).
- **Founding a group allocates**: the group's item vector, a copy of its
  key and a map node (`src/exec/aggregate.cpp:188-201`, spot-read;
  `:371-377`), with nothing reserved and `Reset` a `clear()`
  (`:52-64`). The closed plan's AP05 measured about 1 µs a group founded
  and was not built.
- **`SUM` over `int64`** is one overflow-checked add
  (`src/exec/aggregate.cpp:281-310`); 128-bit arithmetic is
  `DECIMAL128`'s alone.
- **Bounds**: `aggregate_max_groups`, 65,536 by default (AG11,
  `aggregate.md:28`, spot-read); `sort_max_rows` does not apply.

### 1.3 What the fold does not do, as the specs state it `[source-read]`

- No index-only scan: a `COUNT(*)` walks the relation's leaves
  (`index.md` IX7).
- No parallelism: the statement runs whole on its session's core
  (`aggregate.md` §7).
- No spill and no partial answer: a cap refuses (AG11).
- **Groups are emitted in first-seen order** (AG6, `aggregate.md:23`,
  spot-read), which is a contract the group table must keep.

### 1.4 What was measured `[measured]`, each at its commit

- `e81ac7cd`: §0's figures; the fold adds about 71 ns a row to the walk
  (the research pass's reading of the parity file).
- `6c98f7c32`, this host: `close` 1,011.4 µs on the `off` arm, 773.7 µs
  on `on`, 749.8 µs on `declared`
  (`bench/v3.5.0/interim-scenarios-v3.4.17-166-g6c98f7c32.md:151-153`);
  no file says why the arms differ.
- `9f8f8b354`, this host, the archived JSON as the research pass read
  it: over nine `off` day-runs `close` has a p50 of 467 µs to 625 µs and
  a mean of 774 µs to 1,211 µs, so its mean is carried by its largest
  statement.
- PostgreSQL 18.7, this host: `close` mean 474.3 µs, p50 206.6 µs.
- `9682b725` and `a40882af`: `close` at 0.57x, then 0.67x once the
  client's decoder changed and nothing else did.
- **Not measured anywhere**: a per-statement split of `close` on either
  engine; the fold's server CPU on `main`; the share of any item of
  §1.2.

## 2. The mechanism `[design]`

Candidates, named and not chosen; S0's ranking picks.

- **(a) A row the fold reads in place.** Where the fold reads no cell
  (`COUNT(*)`) or fixed-width cells only, the payload is not copied: the
  cells are read under the leaf's hold after the row is classified. The
  copy stays wherever a value must outlive the hold.
- **(b) The sink called directly**, not through `std::function`, on the
  aggregated path.
- **(c) The group table**: reserved from the planner's prior where one
  is published, keyed by the encoded bytes in one arena, with the
  first-seen order kept by position as now.
- **(d) The fold given slices** as a `SelectRun` has them. This is a
  cost to the fold and a gain to every other session on its core; it is
  EH-Q2's.

AG1 says the chain is byte-identical with and without the fold. (a) and
(b) change how a row reaches the fold, not the chain; whether that is
inside AG1 as written is EH-Q3's.

## 3. Rules

- **EH-R1. Every answer is byte-identical**, groups in first-seen order
  (AG6): the aggregate contract suite stays green, one test per numbered
  item, and the PostgreSQL differential (`scripts/bs/difftest.py`) is
  run over the folds.
- **EH-R2. Visibility is decided per row at the statement's snapshot,
  before any cell is read.** A fold that counts a version its snapshot
  does not see, or skips one it does, is `[quiet-wrong]`.
- **EH-R3. A cell read in place is read under the page's pin and latch
  and never after** (invariant 10); nothing of a row is held by pointer
  across a leaf. A value read after its hold is `[quiet-wrong]`.
- **EH-R4. Every refusal stays where it is**: a `SUM` overflow on the
  row that overflows, a cap at the group that exceeds it, a decode's
  `Corruption` on the row that carries it.
- **EH-R5. The unaggregated path is not made slower**: a point `SELECT`
  and a walked `SELECT` are measured interleaved at each stage's end.
- **EH-R6. Measured in server CPU, through equal clients**
  (`kwp.Connection.query`), interleaved A/B in `build-release`, the
  server pinned.
- The standing rules, as DT's order lists them.

## 4. Items for the operator (EH-Q), CLA's proposals

| | item | options | CLA proposes |
|---|---|---|---|
| EH-Q0 | Is EH opened, and with what scope? | (a) S0, then the candidates S0 ranks, one a stage; (b) S0 alone | (a) |
| EH-Q1 | The bar | (a) a global fold's server CPU per row at or below PostgreSQL's on S0's host, each shape stated, and EH closes on what was measured whether or not the bar is met; (b) a fixed cut of S0's figure; (c) no bar | (a) |
| EH-Q2 | Does an aggregated walk yield? | (a) inside EH as its last stage, after the per-row work, with its cost to the fold measured and a cancel taken at its slice boundary as a `SelectRun`'s is; (b) a separate order; (c) not built, and `sched.md` §3 says an aggregate holds its core | (a) |
| EH-Q3 | AG1 | (a) read as "the chain's steps and its row stream are identical": a fold-aware row delivery is inside it, and `aggregate.md` says so; (b) AG1 kept to the letter, which leaves (c) of §2 alone | (a) |
| EH-Q4 | `COUNT(*)` from a structure's counts | (a) not built: a count that is not per row at the snapshot is not the answer; (b) considered | (a) |
| EH-Q5 | The query layer's fold | (a) outside EH; (b) inside | (a) |
| EH-Q6 | Scenario 4's `close` | (a) the driver times each of its four statements as its own phase from S0 on, on EF's equal client if EF-S1 has landed and on `kwp.Connection.query` directly if not; (b) left as one phase | (a) |

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **EH-S0** | The premise on `main`: a global `COUNT(*)`, a global `SUM` and a `GROUP BY` at 1,000 and at 60,000 groups' worth of rows, server CPU per row beside PostgreSQL's in one window; a profile that ranks §1.2 | the ranking in §7; EH-Q1's figure |
| **EH-S1..** | One candidate a stage, in S0's order | each stage's server CPU per row; the suite; EH-R5's pair |
| **EH-Sy** | The slices (EH-Q2 (a)) | a fold that yields and is cancelled at a boundary; its cost |
| **EH-Sz** | The close: `aggregate.md`, `sched.md` §3, `known-gaps.md`, the overhead measurement | the operator's word on landing |

## 6. What EH does not do

- No parallel fold, no batch or vector executor, no SIMD kernel.
- No index-only scan (IX7), no stored or incremental aggregate.
- No native `HAVING`, no order over aggregates, no expression in a
  grouping key.
- No spill: AG11's caps refuse as now.
- Nothing of a read's fixed cost (EF) or a commit's (EG).

## 7. Stage log (CLA, appended as stages land)

### Written 2026-11-28

On `pg-kds-comparison-orders` from `b95be2e86`, on the operator's word.
No stage is opened and no code is written. The 0.47x EH was asked for is
the scenario drivers' figure and carries their unequal clients; the
order's premise is the equal-client measurement at `e81ac7cd`, which S0
repeats on `main`.
