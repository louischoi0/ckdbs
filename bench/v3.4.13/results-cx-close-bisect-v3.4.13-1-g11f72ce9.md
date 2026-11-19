# CX close regression, bisected: the point-statement cost enters at DF-S1, and no CX stage is shown to carry any

On worktree `cx-close-regression-bisect` at `11f72ce9`
(`v3.4.13-1-g11f72ce9`), the 1.2 to 2.0 % a point statement lost over CX's
span (`bench/v3.4.12/results-cx-close-v3.4.12-115-ga32528a4.md`) is
reproduced on `main` as +0.3 to +0.5 µs of server CPU a statement at
`cores = 1`, pooled over four server lifetimes (+0.4 to +0.8 µs in the one
lifetime that has an arm at every stage). **In that lifetime the cost enters
at one step, DF-S1** (`58d258a3` to `ee89842e`, the widened
`sys.statement_stats` bookkeeping): +0.25, +0.57 and +0.79 µs on the three
cells, the only step above both controls on every cell. CX-S0 to CX-S3a
before it are at zero in every design here; CX-S3b to the close and CY
after it move in both directions and sum to -0.01, -0.02 and -0.21 µs. Inside
DF-S1, a spike that compiles its per-statement pieces out says the
compile-time clock costs nothing, and that the session cost sample taken at a
statement's head and end is the likeliest carrier, at a plausible +0.1 to
+0.3 µs - but the spike does not separate it from what a binary's layout
moves, which is 0.2 to 0.3 µs here.

This file closes the item the CX close left open ("a bisect per stage was not
run"; `raft-marks-2026-11-18.md` §47 and `raft-marks-2026-11-19.md` §1, "does
not settle: the regression's bisect"). It withdraws one reading of that file:
its run 3 put +0.29, +0.50 and -0.03 µs between Y (DF-S1) and B (the close);
with 2.5 times the statements and an arm at every stage between, B - Y is
-0.01, -0.02 and -0.21 µs here. That run's own control moved 0.04 to 0.23 µs
over 12 rounds, and its Y and B were one process each.

Driver and raw output: `archive/cx-close-bisect-v3.4.13-1-g11f72ce9/`.
`arms_bench.py` is a new driver, not one of `tools/`: it imports the CX
close's archived `cx_close.py` for its data, cells and block loop and makes
the arms a list. `an.py`, `merge.py`, `life.py` are the analysis, `spike.diff`
the knob spike, `an1.txt` and `an2.txt` the analysis output every table below
is generated from. No PostgreSQL twin: the question is a delta inside this
engine. No number here is set beside another host's.

## Run stamp

| Item | Value |
|---|---|
| Executed | 2026-11-19, 03:48 to 04:42 UTC: builds 03:48 to 03:55 and 04:14 to 04:18; stage 1 03:56 to 04:13; stage 2 04:18 to 04:42, of which 04:23 to 04:30 was lost to another session's build and suite (below) |
| Branch / worktree | `worktree-cx-close-regression-bisect`, worktree `cx-close-regression-bisect`, at `11f72ce9` |
| Arms, existing binaries | A `8b584cf6` (`v3.4.12-36-g8b584cf6`, CX's opening), X `58d258a3` (`v3.4.12-76-g58d258a3`, CX-S0 to S3a; its tree equals `2ca42fea`'s under `src`, `include`), Y `ee89842e` (`v3.4.12-88-gee89842e`, X plus DF-S1; equals `3adf8810`'s), B `a32528a4` (`v3.4.12-115-ga32528a4`, CX's close): the CX close's own binaries from `/home/louis/cxclose/run/`, each sha256 equal to the one that file's run stamp records |
| Arms, built here | S3b `2d297055` (`v3.4.12-108-g2d297055`), S5 `9712f5f0` (`v3.4.12-112-g9712f5f0`), S6 `29401d0f` (`v3.4.12-113-g29401d0f`), S7 `435336c9` (`v3.4.12-114-g435336c9`), M `11f72ce9` (`v3.4.13-1-g11f72ce9`, `main`: B plus CY); `git archive` into `/home/louis/cxbisect/src<commit>`, built outside the worktree (`build.py`) |
| Spike arms | KS, KC, KALL: M's source plus `spike.diff`, one `-D` set each (`buildk.sh`). KS `KDS_SPIKE_NO_SAMPLE`: `BeginStatement` returns at once, so neither the head's nor the end's `FlushOnCpu` and `SampleSessionCost` runs. KC `KDS_SPIKE_NO_COMPILE_NS`: no `NowNs()` at `StageStatement`'s head, `NoteStatementCompiled` a no-op. KALL: both, and `KDS_SPIKE_NO_COST` - `RecordStatement` hands `Record` a default cost, so the cost is neither assembled nor folded into the shape; `Record` still runs and still counts its latency bucket, and the emit paths still fill `rows_returned` and `examined` |
| Build | Release, `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, `kds_server` only, GNU 14.2.0; 76 to 79 s each |
| Binary sha256 | S3b `942e3c41…f47b`, S5 and S6 `7dbf6c2a…3de7` (**one binary**: `29401d0f` changes no server source, and two builds in two directories are byte-identical), S7 `7cb4a5a8…2754`, M `9d531cce…98fd` (full values in `build.out`); KS `cdc63ad0…573e`, KC `cac92f7f…3b91`, KALL `9e4a7f2f…5f08` (`buildk.out`); A `35a1fcf5…6262e`, X `086f64f6…842d9`, Y `45d7b430…f54a`, B `64d41546…7ca9` (full values in the CX close's run stamp) |
| Provenance | Every server started from a copy in `/home/louis/cxbisect/run/` |
| Device | Data files, WAL and logs under `/home/louis/cxbisect/run/wd*/` on `/dev/nvme0n1p1`, ext4 (`df -T` at run time); not tmpfs |
| Host | Intel Xeon Platinum 8581C @ 2.10 GHz, 8 CPUs, Linux 6.12.116+deb13-cloud-amd64, clocksource `tsc`; every server pinned to CPU 2 and queried one at a time, the client to CPU 4 |
| Server config | `durability = group`, `buffer_pool_frames = 65536`, `log_level = warn`, `cores = 1`, every other key default; ports 26701 to 26710 and 26751 to 26756 |
| Host load | In every block's `host` record, taken as the block ends (the driver's order; `bench/README.md` rule 4 says before). Stage 1: 1-minute load 0.97 to 2.49, no block with a `cc1plus`, `cmake --build` or `ctest` process (0 of 900). Stage 2: see "what was cut" |

## Method

One client connection a server, a warm-up of a tenth of the block untimed, the
arm order rotated and reversed on alternate rounds, 20,000 statements a block;
server CPU is the server threads' `schedstat` over the block per statement, as
the CX close. Data and cells as the CX close: `pk-point-read` (`SELECT * FROM
pb_bars WHERE id = <random>`), `point-join-2rel` and `point-join-3rel` (tuple
key chains from that id). The result multiset of every cell was equal on every
arm of every lifetime (`*.check`).

**A delta is the median over rounds of the round-paired difference**, with a
bootstrap 95 % interval of that median (2,000 resamples, seeded per pair,
`an.py`), not the difference of two medians: the arms of one round ran within
about 30 s of each other.

| Stage | What | Arms | Rounds | Time |
|---|---|---|---|---|
| 1 | every stage of the span, one server lifetime | A, X, Y, S3b, S5, A2 (control: a second server of A's binary), S6 (control: a second server of S5's binary), S7, B, M | 30 | 03:56 to 04:13 |
| 2 | the DF-S1 knob spike, a fresh server and data file per arm in each of four lifetimes | A, KALL, M, X, KS, KC | 12 a lifetime (10 kept of the first) | 04:18 to 04:23, 04:30 to 04:42 |

**The floor is a property of the server process, not of the block count.** Two
servers of one binary differ by up to 0.27 µs of CPU over 30 rounds (A2 - A:
+0.04, +0.27, -0.15 µs on the three cells; S6 - S5: -0.19, +0.06, +0.10), and
the intervals of those control pairs exclude zero on two of the six. More
rounds on one pair of processes narrow the interval around that pair's own
offset and no further, which is why stage 2 restarts every server four times
and reports each lifetime beside the pooled figure. Stage 2 has no
same-binary control of its own; the floor it is read against is stage 1's.

## Result: where the cost enters (stage 1)

Round-paired server CPU a statement, median [95 % interval], n = 30 rounds
(600,000 statements an arm and cell):

| Step | What it adds | pk-point-read | point-join-2rel | point-join-3rel |
|---|---|---|---|---|
| A2 - A | nothing (control) | +0.04 µs [-0.15, +0.13] | +0.27 µs [+0.07, +0.43] | -0.15 µs [-0.35, +0.43] |
| S6 - S5 | nothing (control) | -0.19 µs [-0.33, -0.01] | +0.06 µs [-0.22, +0.25] | +0.10 µs [-0.08, +0.27] |
| X - A | CX-S0, S1a, S1b, S2, S3a | +0.23 µs [+0.04, +0.35] | +0.23 µs [+0.12, +0.37] | +0.21 µs [-0.08, +0.46] |
| **Y - X** | **DF-S1** | **+0.25 µs [+0.07, +0.36]** | **+0.57 µs [+0.47, +0.69]** | **+0.79 µs [+0.56, +1.01]** |
| S3b - Y | CX-S3b | +0.23 µs [+0.09, +0.30] | +0.07 µs [-0.02, +0.20] | +0.02 µs [-0.23, +0.35] |
| S5 - S3b | CX-S5 | +0.19 µs [+0.00, +0.26] | -0.12 µs [-0.30, +0.11] | -0.41 µs [-0.64, -0.26] |
| S7 - S5 | CX-S7 (S6 changes no server source) | -0.32 µs [-0.51, -0.20] | -0.26 µs [-0.43, +0.05] | -0.03 µs [-0.25, +0.20] |
| B - S7 | CX-S7's review | -0.00 µs [-0.16, +0.10] | +0.07 µs [-0.04, +0.25] | +0.24 µs [+0.06, +0.47] |
| M - B | CY, the restoring merge | -0.04 µs [-0.16, +0.13] | -0.04 µs [-0.20, +0.15] | -0.02 µs [-0.16, +0.24] |
| B - Y | CX-S3b to the close, summed | -0.01 µs [-0.30, +0.21] | -0.02 µs [-0.42, +0.11] | -0.21 µs [-0.61, +0.10] |
| B - A | the CX close's own delta | +0.42 µs [+0.29, +0.52] | +0.66 µs [+0.48, +0.79] | +0.89 µs [+0.43, +1.05] |
| M - A | the same, on `main` | +0.39 µs [+0.27, +0.53] | +0.60 µs [+0.47, +0.77] | +0.78 µs [+0.51, +1.02] |

Reading. Y - X is the only step above both controls on every cell: on the two
joins it is two to three times the widest control of any cell (0.27 µs) and
five times its own cell's. After Y the steps are not each at the floor - S5 -
S3b on the 3-relation join (-0.41 µs) and S7 - S5 on the point read
(-0.32 µs) are about one and a half times the widest control, both downward
- but they go up and come back, and B - Y is -0.01, -0.02 and -0.21 µs with
upper ends of +0.21, +0.11 and +0.10. So this lifetime puts none of the cost
after DF-S1, and bounds what it could have missed there at about 0.2 µs on
the point read and 0.1 µs on the joins. X - A reads +0.2 µs on all three
cells here and does not survive stage 2's restarts (below): it was this
lifetime's pair of processes. **The placement at DF-S1 is one lifetime's**:
stage 2 has no Y arm, and confirms the two ends of it - X at A, M not.

**Against this engine's previous number for the same shapes**: the CX close
on this host measured B - A at +0.41, +0.57 and +0.74 µs (its run 1) and
+0.51, +0.64 and +0.78 µs (its run 3); this run gives +0.42, +0.66 and +0.89.
The delta is the same one. Its attribution is not: that file's run 3 had Y
at +0.22 and +0.14 µs over A on the point read and the 2-relation join; here
Y is +0.49 and +0.72 over A.

Latency and throughput, stage 1 (median over 30 blocks of each per-block
statistic; n = 600,000 statements an arm and cell):

| Cell | Arm | QPS | p0 | p25 | p50 | p95 | p99 | Server CPU |
|---|---|---|---|---|---|---|---|---|
| pk-point-read | A | 23,883 qps | 34.3 µs | 38.7 µs | 39.4 µs | 52.7 µs | 68.1 µs | 21.44 µs |
| pk-point-read | A2 | 23,909 qps | 34.1 µs | 38.7 µs | 39.4 µs | 52.8 µs | 70.2 µs | 21.46 µs |
| pk-point-read | X | 23,828 qps | 34.2 µs | 38.8 µs | 39.6 µs | 52.8 µs | 70.4 µs | 21.59 µs |
| pk-point-read | Y | 23,606 qps | 35.0 µs | 39.2 µs | 39.8 µs | 53.3 µs | 73.2 µs | 21.82 µs |
| pk-point-read | S3b | 23,453 qps | 35.1 µs | 39.4 µs | 40.2 µs | 54.0 µs | 71.5 µs | 22.06 µs |
| pk-point-read | S5 | 23,366 qps | 35.0 µs | 39.5 µs | 40.2 µs | 54.3 µs | 71.9 µs | 22.23 µs |
| pk-point-read | S6 | 23,444 qps | 34.9 µs | 39.3 µs | 40.1 µs | 54.3 µs | 72.2 µs | 22.08 µs |
| pk-point-read | S7 | 23,642 qps | 34.8 µs | 39.1 µs | 39.8 µs | 53.5 µs | 72.0 µs | 21.91 µs |
| pk-point-read | B | 23,678 qps | 34.7 µs | 39.1 µs | 39.7 µs | 53.5 µs | 71.8 µs | 21.90 µs |
| pk-point-read | M | 23,608 qps | 34.6 µs | 39.1 µs | 39.7 µs | 53.8 µs | 71.8 µs | 21.81 µs |
| point-join-2rel | A | 20,794 qps | 39.9 µs | 44.3 µs | 45.0 µs | 62.7 µs | 78.1 µs | 28.08 µs |
| point-join-2rel | A2 | 20,691 qps | 40.0 µs | 44.6 µs | 45.4 µs | 62.5 µs | 77.3 µs | 28.22 µs |
| point-join-2rel | X | 20,695 qps | 40.2 µs | 44.5 µs | 45.4 µs | 62.5 µs | 76.6 µs | 28.31 µs |
| point-join-2rel | Y | 20,436 qps | 40.6 µs | 45.0 µs | 45.9 µs | 63.2 µs | 77.9 µs | 28.78 µs |
| point-join-2rel | S3b | 20,379 qps | 40.7 µs | 45.3 µs | 46.1 µs | 63.3 µs | 79.2 µs | 28.81 µs |
| point-join-2rel | S5 | 20,398 qps | 40.6 µs | 45.1 µs | 46.0 µs | 63.3 µs | 77.9 µs | 28.77 µs |
| point-join-2rel | S6 | 20,434 qps | 40.6 µs | 45.1 µs | 45.9 µs | 63.3 µs | 77.4 µs | 28.69 µs |
| point-join-2rel | S7 | 20,521 qps | 40.3 µs | 44.9 µs | 45.7 µs | 63.0 µs | 78.1 µs | 28.57 µs |
| point-join-2rel | B | 20,508 qps | 40.6 µs | 45.0 µs | 45.8 µs | 63.2 µs | 77.8 µs | 28.74 µs |
| point-join-2rel | M | 20,510 qps | 40.4 µs | 45.0 µs | 45.7 µs | 63.5 µs | 78.1 µs | 28.63 µs |
| point-join-3rel | A | 18,526 qps | 44.9 µs | 49.4 µs | 50.5 µs | 69.3 µs | 83.4 µs | 33.52 µs |
| point-join-3rel | A2 | 18,553 qps | 45.2 µs | 49.6 µs | 50.6 µs | 69.0 µs | 82.6 µs | 33.42 µs |
| point-join-3rel | X | 18,522 qps | 45.3 µs | 49.7 µs | 50.7 µs | 69.3 µs | 83.2 µs | 33.62 µs |
| point-join-3rel | Y | 18,185 qps | 45.6 µs | 50.3 µs | 51.7 µs | 70.5 µs | 82.9 µs | 34.38 µs |
| point-join-3rel | S3b | 18,199 qps | 45.7 µs | 50.4 µs | 51.7 µs | 70.5 µs | 85.4 µs | 34.36 µs |
| point-join-3rel | S5 | 18,374 qps | 45.7 µs | 50.2 µs | 51.2 µs | 69.7 µs | 83.6 µs | 33.94 µs |
| point-join-3rel | S6 | 18,259 qps | 45.6 µs | 50.3 µs | 51.4 µs | 69.8 µs | 82.8 µs | 34.10 µs |
| point-join-3rel | S7 | 18,396 qps | 45.4 µs | 50.1 µs | 51.1 µs | 69.5 µs | 83.0 µs | 33.84 µs |
| point-join-3rel | B | 18,318 qps | 45.4 µs | 50.2 µs | 51.3 µs | 69.7 µs | 82.3 µs | 34.11 µs |
| point-join-3rel | M | 18,307 qps | 45.2 µs | 50.2 µs | 51.2 µs | 70.2 µs | 83.2 µs | 34.13 µs |

Throughput against A: M -1.1, -1.4 and -1.2 %; B -0.9, -1.4 and -1.1 %; Y
-1.2, -1.7 and -1.8 %; X -0.2, -0.5 and 0.0 %; the control A2 +0.1, -0.5 and
+0.1 %.

## Result: which part of DF-S1 (stage 2)

What DF-S1 put on every statement's path (`git diff 58d258a3 ee89842e --
src/server/command_dispatcher.cpp`): at the head, `BeginStatement` - a
`FlushOnCpu` (one clock read) and a `SampleSessionCost` (a pass over the
session's 19 wait events other than ON_CPU); in `StageStatement`, one more
clock read, and one at `NoteStatementCompiled`; at the end, `RecordStatement`
- a second `FlushOnCpu` and `SampleSessionCost`, the difference of the two
samples, and the cost handed to a wider `StatementStats::Record` (a latency
bucket, the row and page counts, the sample's sum); and on the way a wider
`DispatchOutcome`, a row count in the emit loop and `ExaminedBy`, a sum over
the chain's steps on every executed `SELECT`. KS takes out the sample pair,
KC the two compile-time clock reads, and KALL both and the cost's assembly
and fold; no knob takes out the latency bucket, the wider outcome, the row
count or `ExaminedBy`.

Round-paired server CPU a statement. The pooled column is the median over the
46 kept rounds of four lifetimes with its 95 % interval; the four after it
are each lifetime's own median (10, 12, 12 and 12 rounds):

| Pair | Reads as | Cell | Pooled | 2a | 2c | 2d | 2e |
|---|---|---|---|---|---|---|---|
| M - A | the regression | pk-point-read | +0.32 µs [+0.17, +0.42] | +0.41 µs | +0.22 µs | +0.39 µs | +0.34 µs |
| M - A |  | point-join-2rel | +0.39 µs [+0.24, +0.47] | +0.45 µs | +0.27 µs | +0.45 µs | +0.24 µs |
| M - A |  | point-join-3rel | +0.53 µs [+0.15, +0.67] | +0.22 µs | +0.76 µs | +0.61 µs | +0.26 µs |
| X - A | CX-S0 to S3a | pk-point-read | -0.13 µs [-0.25, -0.01] | -0.17 µs | +0.04 µs | -0.17 µs | -0.31 µs |
| X - A |  | point-join-2rel | +0.09 µs [-0.03, +0.26] | +0.09 µs | +0.12 µs | +0.21 µs | -0.01 µs |
| X - A |  | point-join-3rel | +0.11 µs [-0.08, +0.37] | +0.55 µs | +0.51 µs | -0.27 µs | +0.01 µs |
| M - X | DF-S1 and after | pk-point-read | +0.44 µs [+0.27, +0.54] | +0.51 µs | +0.11 µs | +0.45 µs | +0.56 µs |
| M - X |  | point-join-2rel | +0.31 µs [+0.19, +0.43] | +0.26 µs | +0.37 µs | +0.35 µs | +0.31 µs |
| M - X |  | point-join-3rel | +0.39 µs [+0.22, +0.57] | -0.02 µs | +0.50 µs | +0.57 µs | +0.23 µs |
| M - KS | the head and end sample | pk-point-read | +0.14 µs [+0.01, +0.24] | +0.18 µs | -0.01 µs | +0.25 µs | +0.18 µs |
| M - KS |  | point-join-2rel | +0.22 µs [+0.10, +0.36] | +0.47 µs | +0.18 µs | +0.20 µs | +0.03 µs |
| M - KS |  | point-join-3rel | +0.47 µs [+0.12, +0.59] | +0.27 µs | +0.60 µs | +0.59 µs | +0.20 µs |
| M - KC | the compile-time clock | pk-point-read | -0.01 µs [-0.07, +0.04] | -0.01 µs | -0.08 µs | -0.05 µs | +0.17 µs |
| M - KC |  | point-join-2rel | -0.22 µs [-0.34, -0.04] | -0.33 µs | -0.04 µs | -0.28 µs | -0.25 µs |
| M - KC |  | point-join-3rel | -0.11 µs [-0.46, +0.26] | -0.23 µs | +0.43 µs | -0.16 µs | -0.45 µs |
| M - KALL | all three knobs | pk-point-read | +0.17 µs [-0.00, +0.28] | +0.37 µs | +0.07 µs | +0.19 µs | +0.02 µs |
| M - KALL |  | point-join-2rel | +0.11 µs [-0.13, +0.22] | +0.15 µs | +0.23 µs | +0.08 µs | -0.07 µs |
| M - KALL |  | point-join-3rel | +0.28 µs [+0.03, +0.48] | +0.26 µs | +0.64 µs | +0.40 µs | +0.02 µs |
| KALL - X | what the knobs leave | pk-point-read | +0.31 µs [+0.11, +0.40] | +0.15 µs | -0.00 µs | +0.31 µs | +0.51 µs |
| KALL - X |  | point-join-2rel | +0.30 µs [+0.08, +0.52] | +0.30 µs | +0.03 µs | +0.36 µs | +0.55 µs |
| KALL - X |  | point-join-3rel | +0.05 µs [-0.13, +0.17] | -0.16 µs | -0.46 µs | +0.39 µs | +0.14 µs |

Reading.

- **The regression is there in every lifetime** (M - A positive in all twelve
  lifetime-cells, +0.22 to +0.76 µs) and its pooled size is +0.32, +0.39 and
  +0.53 µs: 1.5, 1.4 and 1.6 % of the 21.2, 27.7 and 33.1 µs of server CPU a
  statement, -0.7, -0.8 and -0.8 % of throughput. That is below stage 1's and
  the CX close's single-lifetime figures (+0.4 to +0.9 µs) and is the better
  estimate of the two, for the reason the method gives.
- **CX-S0 to CX-S3a cost a point statement nothing**: X - A is -0.13, +0.09
  and +0.11 µs pooled, with lifetimes on both sides of zero on every cell.
  Stage 1's +0.2 µs was one pair of processes.
- **The compile-time clock costs nothing measurable**: M - KC is zero or
  negative. KC is the slower binary on the 2-relation join in every lifetime,
  by 0.04 to 0.33 µs, with two clock reads fewer - a binary's own offset, and
  the size of every effect this stage looks for.
- **The session cost sample is the likeliest carrier, and is not separated
  from layout.** Removing it (KS) gives back +0.14, +0.22 and +0.47 µs
  pooled, non-negative in eleven of twelve lifetime-cells. But the first two
  are the size of KC's offset; the third is larger than all of M - X on that
  cell (+0.39 µs), which a part cannot be; and KALL, which removes
  everything KS removes and more, gives back +0.17, +0.11 and +0.28 µs, also
  non-negative in eleven of twelve. The two knob sets agree on a sign and on
  a size of about +0.1 to +0.3 µs for what the sample pair and the cost fold
  carry between them; which of the two, and how much of it is a binary's
  layout, this design cannot say.
- **What the knobs leave is not resolved either.** KALL - X is zero in 2c,
  +0.15 and +0.30 µs on the point read and the 2-relation join in 2a, and
  +0.31 to +0.55 µs on those cells in 2d and 2e. If that is cost and not
  layout, it sits in what no knob removes: the wider `DispatchOutcome`, the
  latency bucket, the emit loop's count, and `ExaminedBy`, which grows with
  the join's width.

Latency and throughput, stage 2 (median over the 46 kept blocks of each
per-block statistic; n = 920,000 statements an arm and cell):

| Cell | Arm | QPS | p0 | p25 | p50 | p95 | p99 | Server CPU |
|---|---|---|---|---|---|---|---|---|
| pk-point-read | A | 24,215 qps | 34.4 µs | 38.8 µs | 39.4 µs | 49.3 µs | 60.9 µs | 21.15 µs |
| pk-point-read | X | 24,362 qps | 34.0 µs | 38.7 µs | 39.2 µs | 48.9 µs | 59.3 µs | 21.04 µs |
| pk-point-read | M | 24,056 qps | 34.5 µs | 39.1 µs | 39.7 µs | 49.4 µs | 60.9 µs | 21.47 µs |
| pk-point-read | KS | 24,090 qps | 34.5 µs | 39.0 µs | 39.6 µs | 49.4 µs | 60.3 µs | 21.35 µs |
| pk-point-read | KC | 24,030 qps | 34.8 µs | 39.1 µs | 39.7 µs | 49.5 µs | 63.1 µs | 21.47 µs |
| pk-point-read | KALL | 24,124 qps | 34.6 µs | 38.9 µs | 39.5 µs | 49.4 µs | 61.4 µs | 21.26 µs |
| point-join-2rel | A | 21,123 qps | 39.9 µs | 44.5 µs | 45.1 µs | 57.3 µs | 69.4 µs | 27.65 µs |
| point-join-2rel | X | 21,069 qps | 40.1 µs | 44.6 µs | 45.2 µs | 57.0 µs | 69.1 µs | 27.72 µs |
| point-join-2rel | M | 20,956 qps | 40.3 µs | 44.9 µs | 45.5 µs | 57.6 µs | 70.2 µs | 28.13 µs |
| point-join-2rel | KS | 21,040 qps | 40.1 µs | 44.6 µs | 45.3 µs | 57.4 µs | 69.1 µs | 27.83 µs |
| point-join-2rel | KC | 20,831 qps | 40.6 µs | 45.1 µs | 45.8 µs | 58.2 µs | 69.4 µs | 28.41 µs |
| point-join-2rel | KALL | 20,994 qps | 40.1 µs | 44.7 µs | 45.3 µs | 57.6 µs | 69.5 µs | 28.05 µs |
| point-join-3rel | A | 18,807 qps | 44.8 µs | 49.6 µs | 50.4 µs | 64.5 µs | 76.7 µs | 33.05 µs |
| point-join-3rel | X | 18,834 qps | 45.3 µs | 49.7 µs | 50.5 µs | 64.5 µs | 77.2 µs | 33.02 µs |
| point-join-3rel | M | 18,656 qps | 45.5 µs | 50.1 µs | 51.0 µs | 64.8 µs | 76.6 µs | 33.55 µs |
| point-join-3rel | KS | 18,794 qps | 45.1 µs | 49.8 µs | 50.6 µs | 64.8 µs | 76.6 µs | 33.14 µs |
| point-join-3rel | KC | 18,558 qps | 45.8 µs | 50.3 µs | 51.1 µs | 65.6 µs | 77.1 µs | 33.66 µs |
| point-join-3rel | KALL | 18,695 qps | 45.4 µs | 49.9 µs | 50.8 µs | 64.7 µs | 76.5 µs | 33.26 µs |

Round-paired p50 against A, for the three cells in order, median [95 %
interval]: M +0.31 µs [+0.18, +0.41], +0.42 µs [+0.25, +0.50], +0.48 µs [+0.34, +0.65]; X -0.15 µs [-0.27, +0.01], +0.06 µs [-0.06, +0.20], +0.04 µs [-0.15, +0.22]; KS +0.26 µs [+0.06, +0.32], +0.13 µs [+0.02, +0.32], +0.13 µs [+0.07, +0.39]; KC +0.31 µs [+0.21, +0.38], +0.64 µs [+0.53, +0.81], +0.64 µs [+0.48, +0.80]; KALL +0.11 µs [+0.02, +0.23], +0.18 µs [+0.11, +0.35], +0.35 µs [+0.28, +0.43]. The delta is in server CPU and shows in p50 at the same
size; p95 and p99 do not separate the arms.

## What this says about the engine

DF-S1 was priced before it was built: the DF-S0 spike put it at +0.11 to
+0.35 µs a point statement at `cores = 1`, every median under DF-Q10 (a)'s
0.5 µs and none resolved from that run's noise
(`bench/v3.4.12/results-df-s0-spike-v3.4.12-69-gb2a4f70a.md`); it priced no
join cell. Built, M - X is +0.44, +0.31 and +0.39 µs pooled: at and above
the top of that forecast. **Against the 0.5 µs bar it is under by the pooled
median and not resolved from it** - the pooled intervals reach +0.54 and
+0.57 µs on the point read and the 3-relation join, and stage 1's
single-lifetime Y - X is over the bar on both joins (+0.57, +0.79 µs).
DF-Q10 (a) returns a part over the bar to the operator; whether this one is
over is the operator's reading, and this file does not settle it.

What the bisect does settle is whose cost it is. **CX's close reported a
regression of CX's span; the span's cost entered with DF-S1, merged into the
span from another work branch**, and nothing here shows a stage CX itself
built - the capture, `sys.plans`, the administrator's plan, the pins, the
one-relation plan's gate, `ADVISE PLAN` - moving a point statement.

The piece that could be made cheaper without giving up a column is the sample
pair. `SampleSessionCost` re-derives the per-class totals from 19 per-event
totals twice a statement, on a statement that waited for nothing, and each
`FlushOnCpu` reads a clock within a few instructions of a `NowNs()` the
dispatcher already took (`began_ns` at the head, `now_ns` at the end). A
session that keeps its class totals where a wait is booked would make the
sample a copy of a few words; a flush handed the dispatcher's instant would
save the two reads only if the wait context's clock and the dispatcher's
are one source, and both are injectable, so that is a condition and not a
given. **This is a proposal for DF, not a measured remedy**: at most +0.1 to
+0.3 µs is there to be bought back, the cheaper pair's own cost is not
measured, and nothing is built.

## What was cut, and what weakens the numbers

- **One of stage 2's five lifetimes was lost and part of another.** Another
  session's build and full suite (`cmake --build build -j8`, then `ctest -j8`,
  in worktree `cz-checkpoint-roll-wait`) ran from about 04:22 to 04:30 UTC:
  1-minute load to 10.2, a competing process in 213 of lifetime 2b's 216
  blocks and in the last two rounds of 2a's (33 blocks). 2b is discarded
  whole (`out2b.json` is in the archive and in no table); 2a keeps its first
  10 rounds (`merge.py` drops a round's cell with a competing process in any
  of its blocks); two lifetimes (2d, 2e) were run in its place once the host
  was quiet. 2c ran straight after that suite ended: no competing process in
  any of its blocks, and a 1-minute load average still decaying from 8.1 to
  1.2 across it.
- **Stage 1 is one lifetime, and the placement at DF-S1 is stage 1's.** Its
  arms are one process each, and its X - A shows what that costs.
- **Stage 2's lifetimes are short** (10 to 12 rounds, 200,000 to 240,000
  statements an arm and cell) and four in number; on the 3-relation join a
  pair's own median moves by 0.4 to 0.9 µs between lifetimes.
- **The spike measures removal, not the pieces in isolation**, and three
  binaries: KS, KC and KALL each carry a layout of their own. A rebuild of
  one source is byte-identical here, so the layout question cannot be asked
  by rebuilding; it would take the pieces switched at run time in one binary.
- **`cores = 1` only.** The CX close's `cores = 2` floor was 2.8 % and it
  resolved nothing there; this run did not try. DF-Q14's `cores = 8` is not
  priced here either.
- **No join-block cell, no write cell.** The CX close found the join-block
  shapes inside the floor. DF-S1 also books every log record a statement
  appends (`BookLogAppend`), which a `SELECT` never reaches: an `INSERT`'s or
  an `UPDATE`'s share of DF-S1 is not measured.
- **No wait decomposition**: a point read waits for nothing, and the unit
  measured is 0.3 to 0.5 µs of CPU on a 21 to 33 µs path.
- **No PMU on this host** (`Performance Events: unsupported p6 CPU model 207
  no PMU driver, software events only`), so no instruction count stands
  behind the timing; every figure is `schedstat` time.
- **Budget, over on both stages.** 54 minutes from the first build to the
  last block, 7 of them the lost lifetime; builds 11 minutes, measured blocks
  34. Stage 1 ran 24 minutes with its builds against the 20 a stage is
  given: ten arms at 30 rounds, where eight would have fitted. Stage 2 ran 28
  minutes with its builds, 21 without the lost lifetime. Nothing was held at
  a quiet gate; the gate was not checked between stage 2's lifetimes, which
  is how 2b was lost, and its time is not a hold.
- **Other sessions' `claude` processes were live** throughout; I did not stop
  them.
