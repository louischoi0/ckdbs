# CB close: the join planner, A/B against the opening commit

The planner (join order chosen by a data-free cost model, range bounds
carried across a join-key equality) makes the three join shapes measured here
21 % to 83 % faster at p50, and costs the three-relation tuple-key point join
15.1 µs per statement (+24.6 % at p50, -20.2 % in QPS), because the point join
compiles every order of its three relations on every execution. The
point read, which has no join, did not move.

Work order: `instructions/v3.2.0/workorder-cb-join-planner.md`. Driver:
`archive/cb-close-v3.2.0-174-g0640941f/cb_close.py` (a one-off for this close;
it is archived beside its data because it is not in `tools/`). No PostgreSQL
twin was run for these shapes: the floor was not measured.

## Run stamp

| Item | Value |
|---|---|
| Executed | 2026-10-18, 10:34 to 10:43 UTC (measured run); builds 10:26 to 10:29 UTC |
| Branch / worktree | B: `worktree-cb-join-planner`, worktree `cb-join-planner`; A: detached worktree of `7e256216` |
| A, the opening commit | `7e256216`, `v3.2.0-172-g7e256216`, clean tree |
| B, the close | `0640941f`, `v3.2.0-174-g0640941f`, clean tree (the results file is committed after the run) |
| Build | Release, `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, `kds_server` only, own build directory per commit (A: its worktree's `build-release`; B: the cb-join-planner worktree's `build-release`); the shared `/home/cdkbs/ckdbs/build-release` was not touched |
| Binary A | sha256 `555d909765977439882592ca9d17847da7fe737bdfb2dc891d015e84e10a0e94`; source binary mtime 2026-10-18 10:28:53 UTC (commit time 07:56:18) |
| Binary B | sha256 `7d93629dd9043826729bceb0317182a8adb7529b4df3fe3cb64824e2e793c7e0`; source binary mtime 2026-10-18 10:28:52 UTC (commit time 10:26:14) |
| Provenance | Both binaries were built after their commit and after the last change at it; every server started from a copy in the run directory |
| Device | Data files, WAL and logs on `/dev/root`, ext4 (`df -T` at run time); not tmpfs |
| Host | AMD EPYC 9V74, 8 CPUs, Linux 7.0.0-1014-azure; servers pinned to CPU 2 (all four on one CPU, queried one at a time), the client to CPU 4 |
| Server config | `cores = 1`, `durability = group`, `buffer_pool_frames = 65536`, `log_level = warn`; every other key default (Waystone on). Arm `Bw` adds `join_order = written` |
| Host load | `/proc/loadavg` and `pgrep -a -f "cc1plus\|cmake --build\|ctest"` recorded with every cell (`out.json`): 1-minute load 0.82 to 1.37 across all 160 cells, no competing build or ctest in any |

Four servers ran side by side, one fresh data file each, identical data:

| Arm | Binary | Setting | Purpose |
|---|---|---|---|
| A | opening commit | default | the baseline |
| B | close | `join_order = planned` (default) | the measurement |
| A2 | opening commit, a second server | default | noise-floor control: nothing differs from A |
| Bw | close | `join_order = written` | B's engine with the planner's order choice off, to separate the compile cost from the other changes |

## Method

Single client, one connection per server, `kwp.Connection.query` (rows
counted, no per-field objects). Each cell is a block of statements run
back to back on one server; every block is preceded by an untimed warm-up of
a tenth of its statements, drawn from a separate generator. The four arms
alternate in 8 rounds, the order rotated by one arm each round and reversed on
even rounds. Statements per block: 5,000 (2,500 for `join-no-literal-rev`,
whose A time is 5 ms), so each reported figure is the median over 8 blocks of
a per-block statistic, 40,000 statements per arm per cell (20,000 for the
reversed join). Latency is the client's wall time around one statement;
server CPU is the sum of every server thread's `schedstat` on-CPU time over
the block divided by its statements (the number no client choice touches).
Row counts per block were equal across the four arms in every cell.

Data (one seed, loaded into all four servers, `BTREE` relations, no secondary
index anywhere):

| Relations | Rows |
|---|---|
| `pb_bars` (tuple key, symbol_id, session_no, 5 prices/volume) | 60,480 (8 symbols x 7,560 sessions) |
| `pb_symbols`, `pb_exchanges` | 8, 2 |
| `pc_users` (id, member_code) | 200 |
| `pc_loans` (id, user_id, book_id), user_id uniform in 1..200 | 10,000 |
| `pc_models`, `pc_results` (model_id 1..8 round robin) | 8, 2,872 |

The scenario drivers were not used for loading: `scenario3_library.py
--loans 10000` would have built 2,000 users, not the 200 this close asked for,
so the relations are this driver's own and are shaped like scenario 3's
`join-no-literal` and scenario 1's `model-join`, not identical to them. The
point cells draw a fresh random key per statement (a different statement
every time); the three join cells run one fixed statement, so Waystone's
replay applies to them identically on every arm and was not switched off.

## Result: throughput, A against B

QPS is the block's statements over its wall time, median over 8 blocks. The
noise floor is A2 against A: the largest gap between two servers of the same
binary was 4.4 % (the reversed join, 5 ms statements) and 1.8 % elsewhere, so
a delta under about 2 % (5 % on the reversed join) is not a finding.

| Cell (rows returned) | A | A2 (control) | B | Bw | B vs A | A2 vs A (floor) |
|---|---|---|---|---|---|---|
| pk point read (1) | 19,948 qps | 20,162 qps | 19,837 qps | 19,926 qps | -0.6 % (inside floor) | +1.1 % |
| 3-relation tuple-key point join (1) | 15,444 qps | 15,715 qps | 12,329 qps | 15,177 qps | **-20.2 %** | +1.8 % |
| join-no-literal, users first (817) | 819 qps | 819 qps | 1,167 qps | 1,180 qps | **+42.5 %** | 0.0 % |
| join-no-literal, loans first (817) | 200 qps | 192 qps | 1,166 qps | 894 qps | **+482 %** | -4.4 % |
| model-join (359) | 2,442 qps | 2,427 qps | 3,099 qps | 2,451 qps | **+26.9 %** | -0.6 % |

## Latency distributions

Client-observed wall latency per statement, median over 8 blocks of each
percentile; "server CPU" is on-CPU time per statement. n is statements per
arm (8 blocks).

| Cell | Arm | n | p0 | p25 | p50 | p95 | p99 | Server CPU |
|---|---|---|---|---|---|---|---|---|
| pk point read | A | 40,000 | 40.2 µs | 47.1 µs | 47.8 µs | 63.2 µs | 72.9 µs | 29.9 µs |
| pk point read | A2 | 40,000 | 40.4 µs | 46.5 µs | 47.2 µs | 61.7 µs | 70.8 µs | 29.1 µs |
| pk point read | B | 40,000 | 40.5 µs | 47.3 µs | 48.0 µs | 63.4 µs | 72.8 µs | 30.2 µs |
| pk point read | Bw | 40,000 | 39.7 µs | 47.0 µs | 47.7 µs | 62.6 µs | 75.0 µs | 29.8 µs |
| point join, 3 rel | A | 40,000 | 50.4 µs | 60.6 µs | 61.6 µs | 78.8 µs | 91.6 µs | 44.4 µs |
| point join, 3 rel | A2 | 40,000 | 49.4 µs | 59.5 µs | 60.4 µs | 79.5 µs | 91.0 µs | 43.2 µs |
| point join, 3 rel | B | 40,000 | 64.2 µs | 75.2 µs | 76.7 µs | 99.0 µs | 111.1 µs | 59.9 µs |
| point join, 3 rel | Bw | 40,000 | 51.0 µs | 61.5 µs | 62.5 µs | 82.3 µs | 93.2 µs | 45.4 µs |
| join-no-literal | A | 40,000 | 1,165.9 µs | 1,208.1 µs | 1,217.1 µs | 1,256.2 µs | 1,334.0 µs | 1,003.9 µs |
| join-no-literal | A2 | 40,000 | 1,173.6 µs | 1,209.8 µs | 1,215.8 µs | 1,249.2 µs | 1,336.2 µs | 1,003.1 µs |
| join-no-literal | B | 40,000 | 811.4 µs | 839.5 µs | 858.9 µs | 890.4 µs | 942.2 µs | 641.1 µs |
| join-no-literal | Bw | 40,000 | 803.9 µs | 829.8 µs | 848.6 µs | 874.7 µs | 930.6 µs | 633.9 µs |
| join-no-literal, loans first | A | 20,000 | 4,862.9 µs | 4,945.5 µs | 4,974.0 µs | 5,080.6 µs | 5,219.9 µs | 4,704.5 µs |
| join-no-literal, loans first | A2 | 20,000 | 5,086.0 µs | 5,170.6 µs | 5,207.7 µs | 5,306.8 µs | 5,500.7 µs | 4,928.0 µs |
| join-no-literal, loans first | B | 20,000 | 813.6 µs | 839.6 µs | 859.7 µs | 885.3 µs | 945.7 µs | 642.0 µs |
| join-no-literal, loans first | Bw | 20,000 | 1,071.6 µs | 1,105.8 µs | 1,113.5 µs | 1,150.8 µs | 1,227.7 µs | 900.4 µs |
| model-join | A | 40,000 | 383.0 µs | 397.8 µs | 405.5 µs | 433.1 µs | 469.3 µs | 264.2 µs |
| model-join | A2 | 40,000 | 384.7 µs | 400.5 µs | 409.0 µs | 436.0 µs | 462.5 µs | 265.5 µs |
| model-join | B | 40,000 | 302.2 µs | 312.5 µs | 318.4 µs | 347.9 µs | 384.9 µs | 178.9 µs |
| model-join | Bw | 40,000 | 383.3 µs | 398.3 µs | 405.7 µs | 428.3 µs | 458.5 µs | 264.1 µs |

B against A at p50 and in server CPU per statement:

| Cell | p50 delta | Server CPU delta | Bw against A, p50 |
|---|---|---|---|
| pk point read | +0.2 µs (+0.3 %), inside floor | +0.3 µs (+1.0 %) | -0.1 µs, inside floor |
| point join, 3 rel | +15.1 µs (+24.6 %) | +15.5 µs (+34.9 %) | +0.9 µs (+1.5 %), inside floor |
| join-no-literal | -358.3 µs (-29.4 %) | -362.7 µs (-36.1 %) | -368.6 µs (-30.3 %) |
| join-no-literal, loans first | -4,114.3 µs (-82.7 %) | -4,062.6 µs (-86.4 %) | -3,860.6 µs (-77.6 %) |
| model-join | -87.1 µs (-21.5 %) | -85.3 µs (-32.3 %) | +0.2 µs, inside floor |

## Where a statement's time goes

These are read-only statements on one connection, so the composition has
three parts: server CPU (parse, compile, execute), the client and loopback
round trip with scheduling, and nothing else. There is no fsync (a read
logs nothing and `group` durability is not reached), no write statement, and
no lock or conflict wait with one client; those wait types are absent, not
unmeasured. The remainder is mean wall time less server CPU per statement.

| Cell | Arm | Mean wall | Server CPU | Remainder (client + round trip + scheduling) |
|---|---|---|---|---|
| pk point read | A | 50.0 µs | 29.9 µs | 20.2 µs (40 %) |
| pk point read | B | 50.3 µs | 30.2 µs | 20.2 µs (40 %) |
| point join, 3 rel | A | 64.7 µs | 44.4 µs | 20.2 µs (31 %) |
| point join, 3 rel | B | 81.0 µs | 59.9 µs | 21.1 µs (26 %) |
| join-no-literal | A | 1,221.2 µs | 1,003.9 µs | 217.3 µs (18 %) |
| join-no-literal | B | 856.6 µs | 641.1 µs | 215.5 µs (25 %) |
| join-no-literal, loans first | A | 4,991.7 µs | 4,704.5 µs | 287.2 µs (6 %) |
| join-no-literal, loans first | B | 857.5 µs | 642.0 µs | 215.6 µs (25 %) |
| model-join | A | 409.3 µs | 264.2 µs | 145.1 µs (35 %) |
| model-join | B | 322.5 µs | 178.9 µs | 143.6 µs (45 %) |

The remainder is the same on A and B for every cell (the reversed join's
287 µs against 216 µs is the 817-row reply being built for a 5 ms statement,
not the planner): the whole delta is in server CPU. The remainder grows with
reply size (20 µs for one row, 216 µs for 817 rows).

## Findings

**1. The point join pays the planner on every execution: +15.5 µs of server
CPU.** In the three-relation point join (`pb_bars -> pb_symbols ->
pb_exchanges`, anchored on `b.id = k`), written order is already the cheapest
order, so the planner's search finds nothing to improve and still compiles all
`3! = 6` orders to find that. The attribution is clean: B with
`join_order = written` (Bw) is 0.9 µs from A, inside the 1.2 µs A2 gap, while
B with the planner on is 15.1 µs above A. The compile overhead is therefore the
planner's search and nothing else the close brought. At 6 orders it is about
2.5 µs per compiled order (inferred from one point, not measured per order).
CB's design (`workorder-cb-join-planner.md` §3.2) names compiling every
permutation up to 4 relations (24 orders) and the greedy order above that; at
4 relations the same shape would pay roughly four times this on the same
inference, which was not measured here. The statement shape this cost lands on
is the engine's cheapest join, the one OLTP point-join traffic is made of: it
takes it from 15,444 qps to 12,329 qps. The work order's expectation that
this compile cost is the main thing to watch is confirmed, and no pruning
or plan cache stands behind it today. The single-relation point read, which is
not a join block, is unchanged (+0.3 %), so the cost is confined to blocks of
two or more relations.

**2. The planner pays for itself on every shape where order or a missing
bound mattered.** `join-no-literal` as written (users first) runs 29 % faster
(1,217 µs to 859 µs) and written the other way round (loans first) 83 % faster
(4,974 µs to 860 µs); the two spellings now return in the same time (859 µs and
860 µs, p50) because both compile to the same chain. `model-join` runs 21.5 %
faster (405.5 µs to 318.4 µs). Every statement returned the same multiset on
A, A2, B and Bw (817, 817, 359 rows; compared once outside the timed blocks),
and the row counts in the timed blocks matched on every arm.

**3. Two mechanisms, separable by the Bw arm.** In `join-no-literal` users
first, Bw is as fast as B (848.6 µs against 858.9 µs, inside noise): the win
there is CB-S1's range carry, not the order. The plan lines show it: B's step
1 gets `filter 0:1.1 >= 1 derived` and `<= 16 derived` on `l.user_id`, and the
build buckets `build_rows=817` rather than `build_rows=10000`
(`build_probes=15` in both). Loans first, with the planner off, Bw is 1,113 µs
(the derived bounds alone turn 10,000 per-row probes of `pc_users` into 817
probes with 44 memo hits, from 4,974 µs) and B is 860 µs: the 253 µs between
them is the order, which makes the 16 users the driver and the filtered
`pc_loans` the built side. In `model-join`, Bw is identical to A (405.7 µs
against 405.5 µs) and the whole 87 µs is the order: B drives from the one
tuple-key lookup `pc_models.id = 3` (derived, from `r.model_id = 3`) and
scans `pc_results` once, where A scans it and probes `pc_models` 359 times
(`memo_hits=358`).

**4. What the engine now does with a join written either way.** The
reversed statement is no longer a trap: the planner turns a 10,000-iteration
probe loop into the same 16-row driver plus single build as the written-well
statement (identical `examined=10858 pages=582` on both spellings). That is
the direction W0 of the work order asked for. The one-sided effect
remains that a statement already written in the best order and carrying no
carried bound (the point join) can only get slower.

## Plans

`ANALYZE <statement>` on each arm, captured before the timed blocks (full text
in `archive/cb-close-v3.2.0-174-g0640941f/plans.json`). Step lines are
execution order; `B` names a written id on each step line and sets
`reordered=1` on the class line when the order moved.

| Cell | A | B | Bw |
|---|---|---|---|
| point join, 3 rel | `Lookup b` -> `Probe s` -> `Probe e`, 3 pages | identical to A (no `reordered`) | identical to A |
| join-no-literal, users first | `Range u [1,16]` -> `Scan l` build on `user_id`, `build_rows=10000 build_probes=15`, `examined=10858 pages=582` | same order; `l.user_id >= 1` and `<= 16` derived; `build_rows=817 build_probes=15`, `examined=10858 pages=582` | identical to B (derived bounds, `build_rows=817`) |
| join-no-literal, loans first | `Scan l` -> `Probe u` x 10,000 (`memo_hits=45`), `examined=20000 pages=10061` | `reordered=1`: `Range u [1,16]` -> `Scan l` build, `build_rows=817`, `examined=10858 pages=582` | `Scan l` (derived bounds on `user_id`) -> `Probe u` x 817 (`memo_hits=44`), `examined=10817 pages=878` |
| model-join | `FilterScan r` (`model_id = 3`, 2,872 examined) -> `Probe m` x 359 (`memo_hits=358`), `pages=380` | `reordered=1`: `Lookup m key=3` (derived) -> `FilterScan r`, `examined=2873 pages=22` | same as A |
| pk point read | `Lookup pb_bars` | identical | identical |

## What the run says about the engine

- **Unplanned order was the larger cost in both directions.** The loans-first
  spelling was 5.8x the users-first one on A (4,974 µs against 859 µs
  reached by the planner); the planner removes the dependence on the spelling
  for inner joins of this size. The mechanical cause is in the plan lines:
  a probe loop of 10,000 opens against a 16-row driver and one build.
- **The point join's cost is compile, not execute.** The B and A plans of
  that statement are line for line identical and touch the same three pages;
  15 µs of server CPU is spent choosing among six orders and ending where it
  started. This is the one place a CB follow-up (a cheaper search for a block
  that is already a chain of tuple-key probes, or a statement-shape memo of
  the chosen order) has measurable room; the data here do not choose between
  those, and CB-Q2 (a)'s "one catalog, one plan" property is what a memo would
  have to keep.
- **The model's constants chose correctly on all four join cells** (the
  best order of the two or three reachable ones each time); that is four
  cases and says nothing about shapes with a real cost tie.

## What was cut and what was not measured

- **Cut**: the 200 / 1K / 10K row-set sweep (README rule 9) was not run: the
  work order asked for one size per cell, so each join cell is one cardinality
  (loans 10,000, results 2,872, bars 60,480). A fixed cost (the compile, in
  finding 1) and a per-row one (the build, in finding 3) are separated here by
  the Bw arm and the plan lines, not by a sweep. The point cells do not scale
  with rows and were run at one size only.
- **Cut**: no 4-relation or 5-relation block, so the 24-order and greedy
  paths' compile cost is not measured; `1.` above infers, it does not report.
- **Cut**: PostgreSQL was not run for these shapes (the floor was not
  measured).
- **Not isolated**: Waystone was left on (default), and the three join cells run
  one fixed statement; whether replay shortens the executed part of those
  cells was not measured. It applies to A, A2, B and Bw alike.
- **Not run**: the test suite. Neither commit was changed by this measurement.
- **Not a baseline of record**: the close's work order cites a 1,090 µs / 4,845
  µs run at `dd3af538` with 10,000 loans and 2,000 users; this run uses 200
  users, so those numbers are not comparable and are not set beside these.
- **Client**: the Python `kwp.Connection.query` adds about 20 µs of fixed round
  trip per statement (the remainder table), which bounds how small a server
  delta shows in wall latency; the point-join delta is large against it, and
  the server CPU column carries the same delta without it.
