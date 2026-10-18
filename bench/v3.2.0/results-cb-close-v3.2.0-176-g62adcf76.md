# CB close, planner skip for tuple-key chains: A/B/C against the opening commit

Skipping the order search for a written chain of tuple-key lookups and probes
removes the three-relation point join's planner cost: its p50 is 63.4 µs against
76.5 µs at the commit before the skip (-13.1 µs), and against the opening
commit it is +1.8 µs in the full run and -0.1 µs in a repeat, which is at the
edge of the noise floor and not distinguishable from zero. The gains of the
planner on the three other join shapes held (within 1 % of the commit before
the skip), and the single-relation point read did not move.

Work order: `instructions/v3.2.0/workorder-cb-join-planner.md`. Driver:
`archive/cb-close-v3.2.0-176-g62adcf76/cb_close2.py` (the previous close's
one-off driver with the fourth arm replaced; the data shapes, cells, block
sizes and statistics are unchanged). No PostgreSQL twin was run for these
shapes: the floor was not measured.

## Run stamp

| Item | Value |
|---|---|
| Executed | 2026-10-18; full run 10:53 to 11:02 UTC, repeat of two cells 11:02 to 11:03 UTC; C built 10:52 UTC |
| Branch / worktree | C: `worktree-cb-join-planner`, worktree `cb-join-planner`; A: detached worktree of `7e256216` (binary saved from the previous close) |
| A, the opening commit | `7e256216`, `v3.2.0-172-g7e256216`, clean tree |
| B, the commit before the skip | `0640941f`, `v3.2.0-174-g0640941f`, clean tree (binary saved from the previous close) |
| C, the measured commit | `62adcf76`, `v3.2.0-176-g62adcf76`, clean tree (commit time 10:52:13 UTC) |
| Build | Release, `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, `kds_server` only; C from the cb-join-planner worktree's `build-release` (the shared `/home/cdkbs/ckdbs/build-release` was not touched) |
| Binary A | sha256 `555d909765977439882592ca9d17847da7fe737bdfb2dc891d015e84e10a0e94` (same hash as the previous close's A); source mtime 2026-10-18 10:28:53 UTC |
| Binary B | sha256 `7d93629dd9043826729bceb0317182a8adb7529b4df3fe3cb64824e2e793c7e0` (same hash as the previous close's B); source mtime 2026-10-18 10:28:52 UTC |
| Binary C | sha256 `f1e8f8cea4a4a3989133d8f2b7d6754c1aa923efa6f992273482573c0aa0fc7e`; source mtime 2026-10-18 10:52:59 UTC, after the commit |
| Provenance | Every binary is newer than its commit and was built at that commit; every server started from a copy in the run directory |
| Device | Data files, WAL and logs on `/dev/root`, ext4 (`df -T` at run time); not tmpfs |
| Host | AMD EPYC 9V74, 8 CPUs, Linux 7.0.0-1014-azure; servers pinned to CPU 2 (queried one at a time), the client to CPU 4 |
| Server config | `cores = 1`, `durability = group`, `buffer_pool_frames = 65536`, `log_level = warn`; every other key default (Waystone on, `join_order = planned`) on all four |
| Host load | 1-minute load recorded per cell (`out2.json`, `out3.json`). Full run: 1.1 to 1.8 from round 2 on, 2.2 to 3.3 in round 1 (the tail of C's build and other sessions' work). Repeat: 0.65 to 0.89. The full run's `competing` flag fired on the 12 round-1 cells because the watcher command line contained the pattern it greps for; no build or ctest ran |

Four servers ran side by side, one fresh data file each, identical data:

| Arm | Binary | Purpose |
|---|---|---|
| A | opening commit | the baseline |
| A2 | opening commit, a second server | noise-floor control |
| B | commit before the skip | the previous close's engine, for the delta the skip made |
| C | measured commit | the measurement |

## Method

As the previous close (`results-cb-close-v3.2.0-174-g0640941f.md`, "Method"
and "Data"): one client connection per server, blocks of 5,000 statements
(2,500 for `join-no-literal-rev`) after an untimed warm-up of a tenth, eight
rounds, the arm order rotated and reversed on alternate rounds, the median over
8 blocks of each per-block statistic, so n = 40,000 statements per arm (20,000
for the reversed join). Latency is the client's wall time per statement; server
CPU is the sum of the server threads' `schedstat` over the block per statement.
Row counts were equal on all four arms in every cell, and the result multiset
of every join statement was equal on A, A2, B and C (compared once outside the
timed blocks). The pk point read and the point join draw a fresh random key per
statement. The `rev` cell is the loans-first spelling of `join-no-literal`.

The pk point read and the point join were then run a second time alone (same
arguments, fresh servers and data files, 11:02 UTC) because C's pk-read p50 in
the full run sat 1 µs above the others and the repeat was cheap. Both runs are
reported; they disagree in sign on that cell.

## Result: throughput

QPS is the block's statements over its wall time, median over 8 blocks.
Noise floor: the A2 gap to A, at most 0.4 % in the full run and 2.0 % (point
join) in the repeat; a delta under about 2 % is not a finding.

| Cell (rows returned) | A | A2 (control) | B | C | C vs A | C vs B | A2 vs A (floor) |
|---|---|---|---|---|---|---|---|
| point join, 3 rel (1), full run | 15,449 qps | 15,506 qps | 12,397 qps | 14,972 qps | -3.1 % | +20.8 % | +0.4 % |
| point join, 3 rel (1), repeat | 15,425 qps | 15,735 qps | 12,424 qps | 15,347 qps | -0.5 % (inside floor) | +23.5 % | +2.0 % |
| pk point read (1), full run | 19,992 qps | 20,009 qps | 19,946 qps | 19,582 qps | -2.1 % | -1.8 % | +0.1 % |
| pk point read (1), repeat | 20,001 qps | 20,042 qps | 19,619 qps | 20,343 qps | +1.7 % | +3.7 % | +0.2 % |
| model-join (359) | 2,454 qps | 2,452 qps | 3,119 qps | 3,086 qps | **+25.8 %** | -1.1 % (inside floor) | -0.1 % |
| join-no-literal, users first (817) | 819 qps | 819 qps | 1,171 qps | 1,179 qps | **+44.0 %** | +0.7 % (inside floor) | 0.0 % |
| join-no-literal, loans first (817) | 200 qps | 200 qps | 1,178 qps | 1,176 qps | **+488 %** | -0.2 % (inside floor) | 0.0 % |

## Latency distributions

Client-observed wall latency per statement, median over 8 blocks of each
percentile; server CPU is on-CPU time per statement. n is statements per arm.

Full run:

| Cell | Arm | n | p0 | p25 | p50 | p95 | p99 | Server CPU |
|---|---|---|---|---|---|---|---|---|
| point join, 3 rel | A | 40,000 | 49.4 µs | 60.6 µs | 61.6 µs | 78.6 µs | 88.9 µs | 44.5 µs |
| point join, 3 rel | A2 | 40,000 | 50.4 µs | 60.6 µs | 61.5 µs | 78.7 µs | 88.9 µs | 44.5 µs |
| point join, 3 rel | B | 40,000 | 63.8 µs | 75.2 µs | 76.5 µs | 97.8 µs | 109.2 µs | 59.8 µs |
| point join, 3 rel | C | 40,000 | 52.0 µs | 62.4 µs | 63.4 µs | 82.2 µs | 93.4 µs | 46.2 µs |
| pk point read | A | 40,000 | 40.0 µs | 47.0 µs | 47.7 µs | 62.0 µs | 71.5 µs | 29.7 µs |
| pk point read | A2 | 40,000 | 40.0 µs | 46.7 µs | 47.4 µs | 62.4 µs | 71.9 µs | 29.5 µs |
| pk point read | B | 40,000 | 40.9 µs | 47.2 µs | 47.8 µs | 62.3 µs | 71.5 µs | 29.6 µs |
| pk point read | C | 40,000 | 41.0 µs | 47.9 µs | 48.7 µs | 63.8 µs | 74.1 µs | 30.4 µs |
| model-join | A | 40,000 | 380.4 µs | 397.1 µs | 405.1 µs | 429.7 µs | 468.7 µs | 263.4 µs |
| model-join | A2 | 40,000 | 382.5 µs | 397.0 µs | 404.9 µs | 430.3 µs | 473.7 µs | 263.8 µs |
| model-join | B | 40,000 | 300.0 µs | 311.7 µs | 317.3 µs | 344.0 µs | 374.3 µs | 179.2 µs |
| model-join | C | 40,000 | 301.4 µs | 313.5 µs | 319.6 µs | 347.9 µs | 378.2 µs | 179.9 µs |
| join-no-literal | A | 40,000 | 1,168.5 µs | 1,205.8 µs | 1,213.9 µs | 1,252.0 µs | 1,353.1 µs | 1,001.8 µs |
| join-no-literal | A2 | 40,000 | 1,168.3 µs | 1,209.1 µs | 1,216.7 µs | 1,252.4 µs | 1,344.3 µs | 1,002.0 µs |
| join-no-literal | B | 40,000 | 806.4 µs | 835.4 µs | 852.3 µs | 885.1 µs | 944.5 µs | 639.6 µs |
| join-no-literal | C | 40,000 | 802.7 µs | 829.9 µs | 849.3 µs | 879.6 µs | 923.4 µs | 636.9 µs |
| join-no-literal, loans first | A | 20,000 | 4,874.9 µs | 4,965.8 µs | 4,984.9 µs | 5,099.3 µs | 5,275.7 µs | 4,710.9 µs |
| join-no-literal, loans first | A2 | 20,000 | 4,861.1 µs | 4,953.0 µs | 4,984.0 µs | 5,084.3 µs | 5,256.3 µs | 4,705.9 µs |
| join-no-literal, loans first | B | 20,000 | 807.8 µs | 831.9 µs | 850.8 µs | 881.2 µs | 938.9 µs | 636.2 µs |
| join-no-literal, loans first | C | 20,000 | 804.0 µs | 829.1 µs | 849.3 µs | 877.8 µs | 943.5 µs | 638.2 µs |

Repeat (the two point cells alone):

| Cell | Arm | n | p0 | p25 | p50 | p95 | p99 | Server CPU |
|---|---|---|---|---|---|---|---|---|
| point join, 3 rel | A | 40,000 | 50.0 µs | 60.5 µs | 61.5 µs | 79.2 µs | 90.4 µs | 44.2 µs |
| point join, 3 rel | A2 | 40,000 | 49.8 µs | 59.1 µs | 59.9 µs | 79.9 µs | 91.7 µs | 42.6 µs |
| point join, 3 rel | B | 40,000 | 63.1 µs | 74.5 µs | 76.1 µs | 96.8 µs | 107.9 µs | 59.1 µs |
| point join, 3 rel | C | 40,000 | 50.4 µs | 60.4 µs | 61.4 µs | 81.8 µs | 94.5 µs | 44.5 µs |
| pk point read | A | 40,000 | 40.6 µs | 46.6 µs | 47.4 µs | 63.1 µs | 74.8 µs | 29.3 µs |
| pk point read | A2 | 40,000 | 39.9 µs | 46.5 µs | 47.3 µs | 62.0 µs | 71.0 µs | 29.0 µs |
| pk point read | B | 40,000 | 40.0 µs | 47.0 µs | 47.8 µs | 63.5 µs | 74.7 µs | 29.6 µs |
| pk point read | C | 40,000 | 39.4 µs | 46.2 µs | 46.8 µs | 61.8 µs | 76.3 µs | 28.7 µs |

Deltas at p50 and in server CPU per statement:

| Cell | C vs A, p50 | C vs A, server CPU | C vs B, p50 | B vs A, p50 (A2 vs A p50) |
|---|---|---|---|---|
| point join, full run | +1.8 µs (+3.0 %) | +1.7 µs (+3.8 %) | -13.1 µs (-17.1 %) | +14.9 µs (-0.1 µs) |
| point join, repeat | -0.1 µs (-0.1 %) | +0.3 µs (+0.7 %) | -14.7 µs (-19.3 %) | +14.6 µs (-1.6 µs) |
| pk point read, full run | +1.0 µs (+2.0 %) | +0.7 µs (+2.4 %) | +0.9 µs (+1.9 %) | +0.1 µs (-0.3 µs) |
| pk point read, repeat | -0.6 µs (-1.3 %) | -0.6 µs (-2.0 %) | -1.0 µs (-2.1 %) | +0.4 µs (-0.1 µs) |
| model-join | -85.5 µs (-21.1 %) | -83.5 µs (-31.7 %) | +2.3 µs (+0.7 %) | -87.8 µs (-0.2 µs) |
| join-no-literal | -364.6 µs (-30.0 %) | -364.9 µs (-36.4 %) | -3.0 µs (-0.4 %) | -361.6 µs (+2.8 µs) |
| join-no-literal, loans first | -4,135.6 µs (-83.0 %) | -4,072.7 µs (-86.5 %) | -1.5 µs (-0.2 %) | -4,134.1 µs (-0.9 µs) |

## Where a statement's time goes

Read-only statements on one connection: server CPU (parse, compile, execute)
plus the client and loopback round trip. No fsync (a read logs nothing), no
write statement and no lock or conflict wait with one client; those wait types
are absent, not unmeasured. Remainder is mean wall less server CPU. Full run
unless marked.

| Cell | Arm | Mean wall | Server CPU | Remainder (client + round trip + scheduling) |
|---|---|---|---|---|
| point join, 3 rel | A | 64.6 µs | 44.5 µs | 20.1 µs (31 %) |
| point join, 3 rel | B | 80.6 µs | 59.8 µs | 20.7 µs (26 %) |
| point join, 3 rel | C | 66.7 µs | 46.2 µs | 20.5 µs (31 %) |
| point join, 3 rel, repeat | C | 65.1 µs | 44.5 µs | 20.5 µs (31 %) |
| pk point read | A | 49.9 µs | 29.7 µs | 20.2 µs (40 %) |
| pk point read | C | 51.0 µs | 30.4 µs | 20.6 µs (40 %) |
| model-join | C | 323.9 µs | 179.9 µs | 144.0 µs (44 %) |
| join-no-literal | C | 848.0 µs | 636.9 µs | 211.1 µs (25 %) |
| join-no-literal, loans first | C | 850.5 µs | 638.2 µs | 212.3 µs (25 %) |

The remainder is the same on every arm (20 µs for one row, 211 to 215 µs for
817 rows, 144 µs for 359); the whole of every delta above is server CPU.

## Findings

**1. The skip removes the planner's cost on the point join.** On B the
three-relation tuple-key point join cost 14.9 to 15.3 µs of server CPU above
A in both runs here (59.8 and 59.1 µs against 44.5 and 44.2 µs), the same
figure the previous close reported at `0640941f` (15.5 µs). On C it is
46.2 µs in the full run and 44.5 µs in the repeat, so C is 13.6 and 14.6 µs
below B on the same data and arm rotation. The plan lines are identical on A, B
and C (`Lookup b` -> `Probe s` -> `Probe e`, `examined=3 pages=3`, no
`reordered`), so what the skip removed is compile work, not a change of plan.

**2. What remains against A is at the floor and not resolved.** C minus A at
p50 is +1.8 µs in the full run and -0.1 µs in the repeat; in server CPU +1.7 µs
and +0.3 µs. A2 minus A is -0.1 µs and -1.6 µs on the same cell, so the two
runs' floor is larger than the repeat's C-A gap and about as large as the full
run's. The data support "at most about 2 µs (3 %) above A" and do not support
a nonzero residual cost of the skip's check; a measurement that separates 1 µs
needs more blocks than 8, which was not run (the stage budget).

**3. The pk point read did not move.** C minus A at p50 is +1.0 µs in the
full run and -0.6 µs in the repeat, opposite signs, each at the size of the
control's own gap between runs; this is the placement noise a server in one
slot of a rotation shows, not a cost of C. The cell is not a join block and
the skip does not run for it.

**4. The planner's gains held.** `model-join` -21.1 % at p50 against A,
`join-no-literal` -30.0 %, loans first -83.0 %, each within 1 % of B (+0.7 %,
-0.4 %, -0.2 %, all inside the floor). The loans-first and users-first
spellings still return in the same time (849.3 µs at p50 on C for both). Plans
for the three join statements on C are line for line those of B recorded in
the previous close (`reordered=1`, derived bounds on `l.user_id` and
`pc_models.id = 3`, `build_rows=817`, `examined=10858 pages=582` and
`examined=2873 pages=22`); full text in `archive/.../plans2.json`.

## What the run says about the engine

- **The planner's search cost is fixed per compile and removable by shape.**
  With the written chain of lookups and probes known to be optimal, the six
  orders' compile (about 14 µs) is not paid, and the chain it returns is the
  same. A join block with a scan or range in it still compiles every order;
  this run does not measure that cost on a block shaped like that (the join
  cells here have two relations, 2 orders) so the 3 relation 6-order
  cost on a non-tuple-key chain remains inferred from the previous close and
  is not re-measured.
- **A point join through the planner is within about 2 µs of the opening
  commit, which had no planner**: 4 % of a 45 µs server time, on a client that adds 20 µs.
  The two-run disagreement on the residual sign says the host's ordinary
  variation is of that size.

## What was cut and what was not measured

- **Cut**: the 200 / 1K / 10K row-set sweep (README rule 9), as in the
  previous close; the cells are the previous run's cardinalities (loans
  10,000, results 2,872, bars 60,480).
- **Cut**: the `Bw` arm (`join_order = written`) of the previous close, replaced
  by C; the skip's effect on a block that mixes lookups and scans, and on 4 and
  5 relation chains, was not measured.
- **Cut**: PostgreSQL was not run; the floor was not measured.
- **Not run**: the test suite. This stage changed no code; `62adcf76` was
  measured as committed.
- **Not isolated**: Waystone left on (default) on all arms, the three join
  cells run one fixed statement.
- **Client**: the Python client adds about 20 µs of round trip per statement,
  which bounds how small a server delta shows in wall latency; the server CPU
  column carries the deltas without it.
