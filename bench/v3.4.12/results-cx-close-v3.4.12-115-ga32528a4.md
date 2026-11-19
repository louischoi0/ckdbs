# CX close: the cost of managed plans over the whole milestone, A against B, B' and a noise control

At the close of CX, a statement that reaches the plan machinery without being
planned - a point read, a tuple key join - is 1.2 to 2.0 % slower at
`cores = 1` than at the commit CX opened from (0.4 to 0.8 µs of server CPU on a
27 to 33 µs statement), and the three join-block shapes are unchanged within the
noise floor. `join_order = planned` on the closing binary (B') is
indistinguishable from the default `fixed` (B) on every cell, so the planned
value gate of work order §6 holds, and the delta the point statements carry
is not the captured-plan table. At `cores = 2` the control moves up to 2.8 %
between two servers of one binary, and the point-cell deltas are not
resolved. Enabling an administrator's plan for an unrelated join shape costs
a point read or a two-relation tuple key join nothing measurable; enabling a
one-relation plan costs a point read about 0.1 to 0.2 µs of CPU (0.5 to
1 %), at the edge of the floor and with the same sign in both runs.

Work order: `instructions/v3.2.0/workorder-cx-managed-plans.md` §6 and the
stage log §8 (no CX stage's overhead was measured before this file). Driver
and raw output: `archive/cx-close-v3.4.12-115-ga32528a4/` (`cx_close.py` is the
CB close driver `bench/v3.2.0/archive/cb-close-v3.2.0-176-g62adcf76/cb_close2.py`
with the arms made a parameter and the data, cells and blocks unchanged; the
commands are `go.sh`). No PostgreSQL twin was run for these shapes: the floor
was not measured. This file carries no number from another host or from the CB
close: that file was measured on a different kernel and CPU, and a point read
there (47.7 µs) and here (39.2 µs) are not the same machine.

## Run stamp

| Item | Value |
|---|---|
| Executed | 2026-11-18, 14:42 to 15:19 UTC (builds 14:40 to 14:50 UTC) |
| Branch / worktree | `worktree-cx-plan-change-option`, worktree `cx-plan-change-option` |
| A, the opening commit | `8b584cf6`, `v3.4.12-36-g8b584cf6` (parent of CX's first code commit `30d15c0a`); `git archive` into `/home/louis/cxclose/srcA`, built outside the worktree |
| B, the measured commit | `a32528a4`, `v3.4.12-115-ga32528a4`, tree clean (commit time 14:38:20 UTC) |
| Attribution arms | X = `58d258a3` (`v3.4.12-76-g58d258a3`: CX-S0 to CX-S3a, before DF-S1); Y = `ee89842e` (`v3.4.12-88-gee89842e`: X plus DF-S1 merged); both `git archive` builds outside the worktree |
| Build | Release (`CMAKE_BUILD_TYPE` read from both caches), `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, `kds_server` only; B from the worktree's `build-release`, rebuilt at 14:40 UTC because the existing binary (13:56 UTC) was older than HEAD |
| Binary A | sha256 `35a1fcf5b9f9d7caa709764f0fee92ec6d37b936cb057fa1ef419de849c6262e`; source mtime 2026-11-18 14:41:27 UTC |
| Binary B | sha256 `64d41546cb72385238719c815ac76925950623678d38bb750426fd573b207ca9`; source mtime 2026-11-18 14:40:55 UTC (after the commit) |
| Binary X | sha256 `086f64f645c5f610b0cde99f4c00507730adc441d52406d5858b4165768842d9`; source mtime 14:50:35 UTC |
| Binary Y | sha256 `45d7b430b5f5cab50f6907e1ff6b53633dfe9d763df0f8f6fc2b5d61e4f6f54a`; source mtime 14:50:36 UTC |
| Provenance | Every binary is newer than its commit and built at it; every server started from a copy in `/home/louis/cxclose/run/` |
| Device | Data files, WAL and logs under `/home/louis/cxclose/run/wd*/` on `/dev/nvme0n1p1`, ext4 (`df -T` at run time); not tmpfs |
| Host | Intel Xeon Platinum 8581C @ 2.10 GHz, 8 CPUs, Linux 6.12.116+deb13-cloud-amd64; servers pinned to CPU 2 (CPUs 2,3 at `cores = 2`), queried one at a time, the client to CPU 4 |
| Server config | `durability = group`, `buffer_pool_frames = 65536`, `log_level = warn`, `cores = 1` (`cores = 2` where stated); every other key default (Waystone on). B: `join_order` unset, so `fixed`. B': the same binary with `join_order = planned`. A, A2, X: no such key |
| Host load | 1-minute load in the block's `host` record (`out*.json`); no block carried a `cc1plus`, `cmake --build` or `ctest` process (0 of 980 blocks). 1-minute load per run, minimum to maximum over its blocks: 1.2 to 3.0 (run 1), 1.2 to 2.7 (run 3), 0.8 to 1.7 (run 4), 0.9 to 1.9 (run 5), 1.0 to 1.9 (run 6), 0.8 to 1.5 (run 7); other sessions' `claude` processes were live and I did not stop them |

The span A to B contains CX-S0 to CX-S7 and one non-CX source change, DF-S1
(`9f9b207b`, `44dddd27`, `ee89842e`): wider `sys.statement_stats` bookkeeping
on every statement, which the DF-S0 spike priced at +0.11 to +0.35 µs a point
statement at `cores = 1` (`results-df-s0-spike-v3.4.12-69-gb2a4f70a.md`). **It
is part of the B - A delta below** and no arm here removes it from B.

## Method

| Run | What | Arms | Blocks | Statements per block | Time |
|---|---|---|---|---|---|
| 1 | the CB close shapes, `cores = 1` | A, B, A2, B' | 8 per arm and cell | 5,000 (2,500 for the reversed join) | 14:42 to 14:48 |
| 3 | attribution, `cores = 1` | A, X, A2, Y, B | 12 | 20,000 | 14:52 to 14:55 |
| 4 | plan-lookup probes, `cores = 1` | P0, P1, P0b, P2 (all B) | 12 | 20,000 | 14:55 to 15:03 |
| 5 | `cores = 2` | A, B, A2, B' | 12 | 20,000 | 15:03 to 15:11 |
| 6 | plan-lookup probes, repeat, point cells only | P0, P1, P0b, P2 | 20 | 30,000 | 15:11 to 15:15 |
| 7 | `cores = 2`, repeat, point cells only | A, B, A2, B' | 20 | 30,000 | 15:15 to 15:19 |

Every run is one client connection per server, a warm-up of a tenth of the
block untimed, the arm order rotated and reversed on alternate rounds, and the
median over blocks of each per-block statistic, as the CB close; n is the sum
of timed statements per arm and cell. Latency is the client's wall time per
statement, server CPU is the sum of the server threads' `schedstat` over the
block per statement, QPS is the block's statements over its wall time (derived
from the same blocks). A fresh server and data file per arm and run; data as
the CB close (`pb_bars` 60,480 rows, `pc_loans` 10,000, `pc_results` 2,872,
`pc_users` 200). The result multiset of every cell was equal on every arm of
every run (compared once outside the timed blocks). A 10-block, 5,000-statement
shakedown of the attribution run preceded run 3 and showed the same signs with
a floor of 1.5 %; it was superseded by run 3 and is not kept.

**Cells.** `pk-point-read`: `SELECT * FROM pb_bars WHERE id = <random>`.
`point-join-2rel`: `pb_bars JOIN pb_symbols` on that id; `point-join-3rel`: the
CB close's three-relation join. `join-no-literal`: `pc_users` (range `id` 1 to
16) joined to `pc_loans`, 817 rows; `-rev` its loans-first spelling;
`model-join`: `pc_results JOIN pc_models`, 359 rows. The first three are tuple
key chains and never reach the join order search or the captured table; the last
three are searched under `planned` and captured under `fixed`.

**Captured informed (§6).** On B the driver ran each searched shape until
`ANALYZE` printed `planned order=captured`, before any timed block, and
re-read it after the last block (`plans1.json` `B_prime`, `plans*.json.final`):
all three searched shapes were `order=captured` on B in run 1, `model-join` in runs 4 (all four servers),
5 and 6, and B' and A printed `order=memo` for them. The
`informed=` bit is not printed: CX-Q19 as built captures only once every
relation of the block has a published prior, so no uninformed capture exists
and the assertion is the capture itself (`workorder-cx-managed-plans.md` §7).

## Result: the span's cost on statements that are not planned (cores = 1)

Run 1, throughput. The A2 control is the noise floor; a delta under it is not a
finding. Floor in this run: at most 0.3 % on the point cells, up to 1.1 % on
the join cells.

| Cell | A | A2 (control) | B (`fixed`) | B' (`planned`) | B vs A | B' vs A | A2 vs A (floor) |
|---|---|---|---|---|---|---|---|
| pk-point-read (1 row) | 24,259 qps | 24,209 qps | 23,785 qps | 23,849 qps | **-2.0 %** | **-1.7 %** | -0.2 % |
| point-join-2rel (1 row) | 21,319 qps | 21,373 qps | 21,056 qps | 20,898 qps | **-1.2 %** | **-2.0 %** | +0.3 % |
| point-join-3rel (1 row) | 19,169 qps | 19,132 qps | 18,820 qps | 18,842 qps | **-1.8 %** | **-1.7 %** | -0.2 % |
| join-no-literal (817 rows) | 1,040 qps | 1,029 qps | 1,039 qps | 1,035 qps | -0.1 % (inside floor) | -0.5 % (inside floor) | -1.0 % |
| join-no-literal-rev (817 rows) | 1,037 qps | 1,037 qps | 1,031 qps | 1,029 qps | -0.5 % (inside floor) | -0.7 % (inside floor) | +0.1 % |
| model-join (359 rows) | 3,089 qps | 3,055 qps | 3,064 qps | 3,045 qps | -0.8 % (inside floor) | -1.4 % | -1.1 % |

Run 3 repeats the three point cells with 240,000 statements an arm (floor:
A2 vs A -0.2 % on pk and 2-rel, -0.9 % on 3-rel) and adds the arms X and Y:

| Cell | A | A2 (control) | X (CX-S0..S3a) | Y (X + DF-S1) | B | X vs A | Y vs A | B vs A | A2 vs A (floor) |
|---|---|---|---|---|---|---|---|---|---|
| pk-point-read | 24,332 qps | 24,283 qps | 24,212 qps | 24,261 qps | 24,014 qps | -0.5 % | -0.3 % | **-1.3 %** | -0.2 % |
| point-join-2rel | 21,168 qps | 21,129 qps | 21,221 qps | 21,124 qps | 20,833 qps | +0.3 % | -0.2 % | **-1.6 %** | -0.2 % |
| point-join-3rel | 19,127 qps | 18,959 qps | 18,960 qps | 18,777 qps | 18,794 qps | -0.9 % (at floor) | **-1.8 %** | **-1.7 %** | -0.9 % |

The delta is in server CPU, not in the wire or the client: B costs +0.41, +0.57
and +0.74 µs more server CPU than A on the three point cells in run 1 and
+0.51, +0.64 and +0.78 µs in run 3, against control differences of at most
0.24 µs. It is the same on B' (+0.42, +0.81, +0.74 µs), so it is not the
captured-plan table, which a tuple key chain never reaches and a point read
reaches only as one relaxed load. In run 3 X (everything before DF-S1) stays
within the floor of A on all three cells, Y (with DF-S1) sits between X and B
(+0.22, +0.14, +0.81 µs CPU over A), and B adds +0.29, +0.50 and -0.03 µs over
Y. The steps are individually at or under the floor and the 5-arm design does
not separate DF-S1 from CX-S3b to CX-S7 more finely than that; what it does
say is that CX-S0 to CX-S3a, the stages that built the capture, the
`sys.plans` row and the administrator's plan, left a point statement inside
the floor. DF-S1's own spike estimate (+0.11 to +0.35 µs) accounts for a part
of the +0.4 to +0.8 µs and not for all of it; the rest is spread over the later
stages and is not attributable by this run. B and B' do not differ from each
other on any of the point cells in runs 1 and 3 (all within 0.3 %).

Latency distributions and server CPU, run 1 (n per arm and cell in the table,
median over blocks of each per-block percentile):

| Cell | Arm | n | QPS | p0 | p25 | p50 | p95 | p99 | Server CPU |
|---|---|---|---|---|---|---|---|---|---|
| pk-point-read | A | 40,000 | 24,259 qps | 36.4 µs | 38.7 µs | 39.2 µs | 48.4 µs | 55.9 µs | 21.2 µs |
| pk-point-read | B | 40,000 | 23,785 qps | 37.9 µs | 39.5 µs | 40.1 µs | 49.6 µs | 59.8 µs | 21.6 µs |
| pk-point-read | A2 | 40,000 | 24,209 qps | 37.3 µs | 38.9 µs | 39.5 µs | 48.4 µs | 58.0 µs | 21.2 µs |
| pk-point-read | Bp | 40,000 | 23,849 qps | 37.8 µs | 39.3 µs | 39.9 µs | 49.4 µs | 58.0 µs | 21.6 µs |
| point-join-2rel | A | 40,000 | 21,319 qps | 42.9 µs | 44.3 µs | 44.9 µs | 55.3 µs | 65.8 µs | 27.6 µs |
| point-join-2rel | B | 40,000 | 21,056 qps | 43.6 µs | 45.0 µs | 45.7 µs | 56.2 µs | 65.3 µs | 28.1 µs |
| point-join-2rel | A2 | 40,000 | 21,373 qps | 42.7 µs | 44.3 µs | 44.8 µs | 55.0 µs | 64.6 µs | 27.3 µs |
| point-join-2rel | Bp | 40,000 | 20,898 qps | 43.2 µs | 45.2 µs | 45.8 µs | 57.0 µs | 68.6 µs | 28.4 µs |
| point-join-3rel | A | 40,000 | 19,169 qps | 47.9 µs | 49.4 µs | 50.0 µs | 61.9 µs | 71.3 µs | 32.6 µs |
| point-join-3rel | B | 40,000 | 18,820 qps | 48.6 µs | 50.2 µs | 50.8 µs | 62.6 µs | 74.6 µs | 33.4 µs |
| point-join-3rel | A2 | 40,000 | 19,132 qps | 47.0 µs | 49.4 µs | 50.0 µs | 61.2 µs | 71.2 µs | 32.6 µs |
| point-join-3rel | Bp | 40,000 | 18,842 qps | 48.6 µs | 50.2 µs | 50.8 µs | 63.5 µs | 73.5 µs | 33.4 µs |
| join-no-literal | A | 40,000 | 1,040 qps | 835.9 µs | 908.6 µs | 944.1 µs | 1097.8 µs | 1190.4 µs | 675.9 µs |
| join-no-literal | B | 40,000 | 1,039 qps | 840.0 µs | 910.3 µs | 944.5 µs | 1102.0 µs | 1174.5 µs | 676.2 µs |
| join-no-literal | A2 | 40,000 | 1,029 qps | 837.5 µs | 912.2 µs | 951.0 µs | 1117.8 µs | 1211.2 µs | 684.2 µs |
| join-no-literal | Bp | 40,000 | 1,035 qps | 839.2 µs | 913.6 µs | 949.2 µs | 1105.9 µs | 1193.5 µs | 680.3 µs |
| join-no-literal-rev | A | 20,000 | 1,037 qps | 840.8 µs | 912.0 µs | 946.8 µs | 1103.2 µs | 1167.0 µs | 679.1 µs |
| join-no-literal-rev | B | 20,000 | 1,031 qps | 842.1 µs | 917.5 µs | 951.6 µs | 1110.1 µs | 1179.8 µs | 682.1 µs |
| join-no-literal-rev | A2 | 20,000 | 1,037 qps | 843.3 µs | 911.0 µs | 944.0 µs | 1105.0 µs | 1197.3 µs | 677.0 µs |
| join-no-literal-rev | Bp | 20,000 | 1,029 qps | 839.0 µs | 915.3 µs | 950.9 µs | 1113.7 µs | 1199.2 µs | 681.9 µs |
| model-join | A | 40,000 | 3,089 qps | 296.9 µs | 308.5 µs | 314.4 µs | 372.3 µs | 478.4 µs | 164.7 µs |
| model-join | B | 40,000 | 3,064 qps | 300.1 µs | 310.8 µs | 316.5 µs | 375.9 µs | 476.0 µs | 167.9 µs |
| model-join | A2 | 40,000 | 3,055 qps | 297.2 µs | 308.3 µs | 314.8 µs | 390.0 µs | 503.6 µs | 167.2 µs |
| model-join | Bp | 40,000 | 3,045 qps | 300.3 µs | 312.3 µs | 318.8 µs | 390.9 µs | 516.9 µs | 168.8 µs |

Deltas against A and against the control, run 1:

| Cell | Arm vs A: QPS | p50 | p99 | Server CPU | Control A2 vs A: QPS | p50 | CPU |
|---|---|---|---|---|---|---|---|
| pk-point-read | B: -2.0 % | +0.9 µs | +3.8 µs | +0.41 µs | -0.2 % | +0.3 µs | +0.01 µs |
| pk-point-read | Bp: -1.7 % | +0.7 µs | +2.1 µs | +0.42 µs | -0.2 % | +0.3 µs | +0.01 µs |
| point-join-2rel | B: -1.2 % | +0.7 µs | -0.4 µs | +0.57 µs | +0.3 % | -0.1 µs | -0.24 µs |
| point-join-2rel | Bp: -2.0 % | +0.9 µs | +2.8 µs | +0.81 µs | +0.3 % | -0.1 µs | -0.24 µs |
| point-join-3rel | B: -1.8 % | +0.8 µs | +3.3 µs | +0.74 µs | -0.2 % | -0.0 µs | -0.08 µs |
| point-join-3rel | Bp: -1.7 % | +0.8 µs | +2.3 µs | +0.74 µs | -0.2 % | -0.0 µs | -0.08 µs |
| join-no-literal | B: -0.1 % | +0.4 µs | -15.9 µs | +0.25 µs | -1.0 % | +6.9 µs | +8.29 µs |
| join-no-literal | Bp: -0.5 % | +5.1 µs | +3.0 µs | +4.36 µs | -1.0 % | +6.9 µs | +8.29 µs |
| join-no-literal-rev | B: -0.5 % | +4.7 µs | +12.8 µs | +2.99 µs | +0.1 % | -2.8 µs | -2.05 µs |
| join-no-literal-rev | Bp: -0.7 % | +4.0 µs | +32.2 µs | +2.78 µs | +0.1 % | -2.8 µs | -2.05 µs |
| model-join | B: -0.8 % | +2.1 µs | -2.4 µs | +3.20 µs | -1.1 % | +0.3 µs | +2.56 µs |
| model-join | Bp: -1.4 % | +4.4 µs | +38.5 µs | +4.11 µs | -1.1 % | +0.3 µs | +2.56 µs |

Run 3, the attribution run:

| Cell | Arm | n | QPS | p0 | p25 | p50 | p95 | p99 | Server CPU |
|---|---|---|---|---|---|---|---|---|---|
| pk-point-read | A | 240,000 | 24,332 qps | 34.5 µs | 38.8 µs | 39.4 µs | 48.5 µs | 57.6 µs | 21.1 µs |
| pk-point-read | X | 240,000 | 24,212 qps | 34.4 µs | 38.9 µs | 39.4 µs | 49.1 µs | 62.1 µs | 21.4 µs |
| pk-point-read | A2 | 240,000 | 24,283 qps | 34.5 µs | 38.8 µs | 39.4 µs | 48.7 µs | 58.0 µs | 21.2 µs |
| pk-point-read | Y | 240,000 | 24,261 qps | 34.5 µs | 39.1 µs | 39.6 µs | 48.5 µs | 58.0 µs | 21.3 µs |
| pk-point-read | B | 240,000 | 24,014 qps | 35.0 µs | 39.3 µs | 39.9 µs | 49.1 µs | 57.8 µs | 21.6 µs |
| point-join-2rel | A | 240,000 | 21,168 qps | 39.8 µs | 44.5 µs | 45.2 µs | 56.5 µs | 65.8 µs | 27.6 µs |
| point-join-2rel | X | 240,000 | 21,221 qps | 40.1 µs | 44.5 µs | 45.1 µs | 56.0 µs | 65.8 µs | 27.7 µs |
| point-join-2rel | A2 | 240,000 | 21,129 qps | 40.2 µs | 44.4 µs | 45.1 µs | 56.5 µs | 66.3 µs | 27.6 µs |
| point-join-2rel | Y | 240,000 | 21,124 qps | 40.3 µs | 44.7 µs | 45.3 µs | 56.2 µs | 66.6 µs | 27.8 µs |
| point-join-2rel | B | 240,000 | 20,833 qps | 40.4 µs | 45.2 µs | 45.8 µs | 57.0 µs | 66.3 µs | 28.3 µs |
| point-join-3rel | A | 240,000 | 19,127 qps | 44.8 µs | 49.4 µs | 50.0 µs | 62.7 µs | 73.1 µs | 32.5 µs |
| point-join-3rel | X | 240,000 | 18,960 qps | 45.3 µs | 49.8 µs | 50.4 µs | 63.1 µs | 73.5 µs | 32.9 µs |
| point-join-3rel | A2 | 240,000 | 18,959 qps | 44.9 µs | 49.5 µs | 50.3 µs | 63.1 µs | 73.5 µs | 32.8 µs |
| point-join-3rel | Y | 240,000 | 18,777 qps | 45.6 µs | 50.0 µs | 50.8 µs | 64.2 µs | 75.3 µs | 33.4 µs |
| point-join-3rel | B | 240,000 | 18,794 qps | 45.4 µs | 50.3 µs | 50.9 µs | 63.4 µs | 74.4 µs | 33.3 µs |

| Cell | Arm vs A: QPS | p50 | p99 | Server CPU | Control A2 vs A: QPS | p50 | CPU |
|---|---|---|---|---|---|---|---|
| pk-point-read | X: -0.5 % | +0.0 µs | +4.5 µs | +0.27 µs | -0.2 % | +0.0 µs | +0.06 µs |
| pk-point-read | Y: -0.3 % | +0.2 µs | +0.3 µs | +0.22 µs | -0.2 % | +0.0 µs | +0.06 µs |
| pk-point-read | B: -1.3 % | +0.5 µs | +0.2 µs | +0.51 µs | -0.2 % | +0.0 µs | +0.06 µs |
| point-join-2rel | X: +0.3 % | -0.1 µs | -0.0 µs | +0.05 µs | -0.2 % | -0.1 µs | -0.04 µs |
| point-join-2rel | Y: -0.2 % | +0.1 µs | +0.8 µs | +0.14 µs | -0.2 % | -0.1 µs | -0.04 µs |
| point-join-2rel | B: -1.6 % | +0.6 µs | +0.6 µs | +0.64 µs | -0.2 % | -0.1 µs | -0.04 µs |
| point-join-3rel | X: -0.9 % | +0.4 µs | +0.4 µs | +0.40 µs | -0.9 % | +0.3 µs | +0.23 µs |
| point-join-3rel | Y: -1.8 % | +0.7 µs | +2.2 µs | +0.81 µs | -0.9 % | +0.3 µs | +0.23 µs |
| point-join-3rel | B: -1.7 % | +0.9 µs | +1.3 µs | +0.78 µs | -0.9 % | +0.3 µs | +0.23 µs |

**The join-block shapes show no cost.** B is -0.1, -0.5 and -0.8 % against A on
the 817-row and 359-row shapes, inside the floor in each (the control moved -1.0
and -1.1 % on two of them); B' is -0.5, -0.7 and -1.4 %, and the one cell
beyond its control (model-join, B', 0.3 % past a control of -1.1 %) is 4 µs of p50 on 315 µs. The searched
shapes run the captured (B) or memoised (B') order at the cost of one probe
or one memo hit. The server CPU of the join cells has a control spread of up to
8 µs on 680 µs (1.2 %), which is the resolution of those cells; a 0.5 to 1 µs
shift on them is below it.

## Result: the plan-lookup probes (CX-R6, `known-gaps.md`)

Four servers of the B binary, one knob each against P0: P0 has no plan; P0b
is a second P0, the control; P1 has an enabled administrator's plan for an
unrelated join shape (`CREATE PLAN px_join FOR SELECT l.book_id, u.member_code
FROM pc_users AS u JOIN pc_loans AS l ON l.user_id = u.id WHERE u.id BETWEEN 1
AND 16 VARIANT v ELSE ORDER (u, l)`, so `any()` is true and every join block,
tuple key chains included, probes the table); P2 has an enabled one-relation
plan for an unrelated statement (`CREATE PLAN px_one FOR SELECT * FROM
pc_users WHERE member_code = 'member00005' VARIANT v ELSE ORDER (pc_users)`,
so `any_single()` is true and every top-level one-relation `SELECT`, the point
read included, probes). `SHOW PLANS` on each server showed the plan enabled
(`plans4.json`). The statements measured touch no plan's shape, so no probe
hits and no statement is compiled twice: this is the cost of the miss. P0 is B
with nothing enabled; the cost of "none enabled" against A is the B - A delta
above, which this design cannot separate further.

Run 4 (12 blocks of 20,000), and run 6 (20 blocks of 30,000, point cells only)
as the repeat:

| Cell | Run | P0 (none) | P0b (control) | P1 (join plan enabled) | P2 (one-relation plan enabled) | P1 vs P0 | P2 vs P0 | P0b vs P0 (floor) |
|---|---|---|---|---|---|---|---|---|
| pk-point-read | 4 | 24,139 qps | 24,078 qps | 24,081 qps | 24,004 qps | -0.2 % (inside floor) | -0.6 % | -0.2 % |
| pk-point-read | 6 | 24,085 qps | 24,053 qps | 23,991 qps | 23,855 qps | -0.4 % | **-1.0 %** | -0.1 % |
| point-join-2rel | 4 | 20,932 qps | 20,940 qps | 21,119 qps | 20,916 qps | +0.9 % (inside run's spread) | -0.1 % | +0.0 % |
| point-join-2rel | 6 | 20,965 qps | 20,880 qps | 20,778 qps | 20,939 qps | -0.9 % | -0.1 % | -0.4 % |
| model-join (359 rows) | 4 | 3,038 qps | 2,988 qps | 3,047 qps | 2,956 qps | +0.3 % | -2.7 % (control -1.6 %) | -1.6 % |

Server CPU, the clearer axis (a statement's CPU, µs): pk-point-read P2 - P0 is
+0.17 µs (run 4) and +0.25 µs (run 6) against a control of +0.06 and +0.11 µs;
P1 - P0 is +0.09 µs in both runs. point-join-2rel P1 - P0 is -0.21 and +0.38 µs
(the two runs disagree in sign; control -0.02 and +0.11 µs) and P2 - P0 is
0.00 and +0.04 µs. model-join in run 4 has a control that itself moved +3.0 µs
of CPU and 2.5 µs of p50, so its P1 and P2 rows are noise.

Reading. A join plan enabled for an unrelated shape adds nothing resolvable to
a point read or a two-relation tuple key join (at most 0.4 µs of CPU, the
control's own spread). **A one-relation plan enabled for an unrelated statement
costs a point read about 0.1 to 0.15 µs of CPU over the control, 0.2 to 0.4 µs
of p50, 0.4 to 0.9 % of throughput**: the same sign in both runs and at the
edge of the floor, which is the partition-latch probe `known-gaps.md` predicts
(the entry "A one-relation plan puts every one-relation `SELECT` through the
plan table"). It is real but small; the measurement does not support "zero
cost" for that state, and CX-R6 as it reads (zero for the unmanaged point
path) holds, at this resolution, for the state with none enabled and is
bounded by +0.25 µs for a point read with one one-relation plan enabled. A
point read's 21.3 µs of server CPU is the denominator. Percentiles and server
CPU, run 4:

| Cell | Arm | n | QPS | p0 | p25 | p50 | p95 | p99 | Server CPU |
|---|---|---|---|---|---|---|---|---|---|
| pk-point-read | P0 | 240,000 | 24,139 qps | 35.7 µs | 39.1 µs | 39.6 µs | 48.5 µs | 57.9 µs | 21.4 µs |
| pk-point-read | P1 | 240,000 | 24,081 qps | 35.2 µs | 39.2 µs | 39.8 µs | 48.5 µs | 57.1 µs | 21.5 µs |
| pk-point-read | P0b | 240,000 | 24,078 qps | 35.7 µs | 39.3 µs | 39.9 µs | 48.3 µs | 57.6 µs | 21.5 µs |
| pk-point-read | P2 | 240,000 | 24,004 qps | 35.8 µs | 39.3 µs | 39.9 µs | 48.5 µs | 58.2 µs | 21.6 µs |
| point-join-2rel | P0 | 240,000 | 20,932 qps | 41.7 µs | 45.1 µs | 45.8 µs | 55.9 µs | 66.3 µs | 28.3 µs |
| point-join-2rel | P1 | 240,000 | 21,119 qps | 40.4 µs | 44.8 µs | 45.4 µs | 55.6 µs | 65.4 µs | 28.1 µs |
| point-join-2rel | P0b | 240,000 | 20,940 qps | 41.2 µs | 45.0 µs | 45.7 µs | 56.1 µs | 66.6 µs | 28.3 µs |
| point-join-2rel | P2 | 240,000 | 20,916 qps | 40.9 µs | 45.1 µs | 45.7 µs | 56.3 µs | 65.5 µs | 28.3 µs |
| model-join | P0 | 240,000 | 3,038 qps | 299.3 µs | 311.4 µs | 317.8 µs | 389.1 µs | 521.9 µs | 169.4 µs |
| model-join | P1 | 240,000 | 3,047 qps | 299.4 µs | 311.1 µs | 317.4 µs | 386.4 µs | 519.0 µs | 168.3 µs |
| model-join | P0b | 240,000 | 2,988 qps | 299.8 µs | 312.6 µs | 320.3 µs | 411.3 µs | 543.0 µs | 172.4 µs |
| model-join | P2 | 240,000 | 2,956 qps | 299.7 µs | 313.1 µs | 321.8 µs | 425.7 µs | 529.9 µs | 174.0 µs |

Run 6:

| Cell | Arm | n | QPS | p0 | p25 | p50 | p95 | p99 | Server CPU |
|---|---|---|---|---|---|---|---|---|---|
| pk-point-read | P0 | 600,000 | 24,085 qps | 35.4 µs | 39.3 µs | 39.9 µs | 48.3 µs | 58.5 µs | 21.3 µs |
| pk-point-read | P1 | 600,000 | 23,991 qps | 35.8 µs | 39.4 µs | 40.0 µs | 48.5 µs | 57.2 µs | 21.4 µs |
| pk-point-read | P0b | 600,000 | 24,053 qps | 35.7 µs | 39.4 µs | 40.0 µs | 48.3 µs | 57.6 µs | 21.5 µs |
| pk-point-read | P2 | 600,000 | 23,855 qps | 36.2 µs | 39.7 µs | 40.2 µs | 48.7 µs | 57.8 µs | 21.6 µs |
| point-join-2rel | P0 | 600,000 | 20,965 qps | 40.9 µs | 45.1 µs | 45.7 µs | 56.1 µs | 66.6 µs | 28.1 µs |
| point-join-2rel | P1 | 600,000 | 20,778 qps | 41.1 µs | 45.4 µs | 46.0 µs | 56.6 µs | 67.2 µs | 28.4 µs |
| point-join-2rel | P0b | 600,000 | 20,880 qps | 40.6 µs | 45.1 µs | 45.8 µs | 56.4 µs | 66.9 µs | 28.2 µs |
| point-join-2rel | P2 | 600,000 | 20,939 qps | 40.7 µs | 45.2 µs | 45.8 µs | 55.8 µs | 66.3 µs | 28.1 µs |

## Result: `cores = 2`

Not resolved. Run 5 (12 blocks of 20,000): B against A is -1.2 % on the point
read and -3.1 % on the 2-rel join while B' is +0.1 % and -0.1 %, and the control
is -0.3 % and -0.1 %. Run 7 (20 blocks of 30,000, repeat) gives B +1.1 % and
+0.2 %, B' +1.2 % and +0.7 %, with the control at **+2.8 %** and +0.9 %: one
server of one binary ran 2.8 % faster than the other on the point read.
The run-5 B result is not reproduced by run 7 and B' does not carry it, so I
report no cores = 2 regression and no cores = 2 absence of one; the floor is
2.8 %, wider than the 1.2 to 2.0 % this file finds at `cores = 1`.

Run 5 (every cell; the model-join row is the join-block shape):

| Cell | Arm | n | QPS | p0 | p25 | p50 | p95 | p99 | Server CPU |
|---|---|---|---|---|---|---|---|---|---|
| pk-point-read | A | 240,000 | 23,466 qps | 37.9 µs | 39.9 µs | 40.7 µs | 48.6 µs | 63.9 µs | 24.3 µs |
| pk-point-read | B | 240,000 | 23,178 qps | 38.3 µs | 40.5 µs | 41.4 µs | 49.4 µs | 64.2 µs | 24.6 µs |
| pk-point-read | A2 | 240,000 | 23,394 qps | 38.1 µs | 40.1 µs | 41.0 µs | 48.9 µs | 64.3 µs | 24.4 µs |
| pk-point-read | Bp | 240,000 | 23,499 qps | 36.0 µs | 40.2 µs | 40.8 µs | 49.9 µs | 60.1 µs | 23.5 µs |
| point-join-2rel | A | 240,000 | 20,331 qps | 43.6 µs | 46.2 µs | 47.2 µs | 56.7 µs | 72.0 µs | 31.4 µs |
| point-join-2rel | B | 240,000 | 19,701 qps | 45.0 µs | 47.4 µs | 48.3 µs | 58.4 µs | 75.4 µs | 32.8 µs |
| point-join-2rel | A2 | 240,000 | 20,320 qps | 43.3 µs | 46.3 µs | 47.3 µs | 56.4 µs | 71.1 µs | 31.4 µs |
| point-join-2rel | Bp | 240,000 | 20,317 qps | 41.3 µs | 46.5 µs | 47.2 µs | 58.0 µs | 67.5 µs | 30.6 µs |
| model-join | A | 240,000 | 2,934 qps | 302.0 µs | 314.8 µs | 322.6 µs | 452.9 µs | 545.7 µs | 180.4 µs |
| model-join | B | 240,000 | 2,923 qps | 304.1 µs | 316.1 µs | 323.8 µs | 449.0 µs | 543.1 µs | 180.7 µs |
| model-join | A2 | 240,000 | 2,931 qps | 301.8 µs | 314.0 µs | 321.8 µs | 455.0 µs | 549.4 µs | 179.1 µs |
| model-join | Bp | 240,000 | 2,936 qps | 305.1 µs | 318.7 µs | 326.1 µs | 418.1 µs | 550.3 µs | 180.9 µs |

| Cell | Arm vs A: QPS | p50 | p99 | Server CPU | Control A2 vs A: QPS | p50 | CPU |
|---|---|---|---|---|---|---|---|
| pk-point-read | B: -1.2 % | +0.6 µs | +0.4 µs | +0.36 µs | -0.3 % | +0.2 µs | +0.16 µs |
| pk-point-read | Bp: +0.1 % | +0.0 µs | -3.8 µs | -0.79 µs | -0.3 % | +0.2 µs | +0.16 µs |
| point-join-2rel | B: -3.1 % | +1.1 µs | +3.4 µs | +1.32 µs | -0.1 % | +0.1 µs | +0.01 µs |
| point-join-2rel | Bp: -0.1 % | -0.0 µs | -4.5 µs | -0.85 µs | -0.1 % | +0.1 µs | +0.01 µs |
| model-join | B: -0.4 % | +1.2 µs | -2.6 µs | +0.26 µs | -0.1 % | -0.8 µs | -1.34 µs |
| model-join | Bp: +0.1 % | +3.5 µs | +4.6 µs | +0.45 µs | -0.1 % | -0.8 µs | -1.34 µs |

Run 7:

| Cell | Arm | n | QPS | p0 | p25 | p50 | p95 | p99 | Server CPU |
|---|---|---|---|---|---|---|---|---|---|
| pk-point-read | A | 600,000 | 23,352 qps | 36.6 µs | 40.0 µs | 40.9 µs | 49.1 µs | 64.5 µs | 24.2 µs |
| pk-point-read | B | 600,000 | 23,600 qps | 35.6 µs | 40.1 µs | 40.7 µs | 49.6 µs | 59.4 µs | 23.2 µs |
| pk-point-read | A2 | 600,000 | 24,007 qps | 34.9 µs | 39.5 µs | 40.0 µs | 48.7 µs | 58.2 µs | 22.7 µs |
| pk-point-read | Bp | 600,000 | 23,629 qps | 35.5 µs | 40.0 µs | 40.6 µs | 49.4 µs | 59.2 µs | 23.2 µs |
| point-join-2rel | A | 600,000 | 20,348 qps | 42.6 µs | 46.3 µs | 47.2 µs | 56.3 µs | 71.5 µs | 31.3 µs |
| point-join-2rel | B | 600,000 | 20,394 qps | 41.5 µs | 46.4 µs | 47.0 µs | 57.9 µs | 68.4 µs | 30.5 µs |
| point-join-2rel | A2 | 600,000 | 20,527 qps | 41.2 µs | 45.9 µs | 46.6 µs | 57.7 µs | 68.6 µs | 30.1 µs |
| point-join-2rel | Bp | 600,000 | 20,490 qps | 41.5 µs | 46.2 µs | 46.8 µs | 57.6 µs | 68.1 µs | 30.3 µs |

| Cell | Arm vs A: QPS | p50 | p99 | Server CPU | Control A2 vs A: QPS | p50 | CPU |
|---|---|---|---|---|---|---|---|
| pk-point-read | B: +1.1 % | -0.2 µs | -5.1 µs | -0.98 µs | +2.8 % | -0.9 µs | -1.53 µs |
| pk-point-read | Bp: +1.2 % | -0.3 µs | -5.4 µs | -1.02 µs | +2.8 % | -0.9 µs | -1.53 µs |
| point-join-2rel | B: +0.2 % | -0.2 µs | -3.1 µs | -0.86 µs | +0.9 % | -0.7 µs | -1.26 µs |
| point-join-2rel | Bp: +0.7 % | -0.4 µs | -3.5 µs | -1.02 µs | +0.9 % | -0.7 µs | -1.26 µs |

## What was cut, and what weakens the numbers

- **The empty-snapshot capture (§6's "the one thing `fixed` gives up") was not
  measured, and cannot be as written**: no capture is made from an empty
  snapshot (CX-Q19 as built). What exists is a deferral. On B right after the
  load, the first compile of `join-no-literal` printed `order=written
  capture=deferred`, and `-rev` and `model-join` `order=planned capture=deferred`
  (searched, uncaptured); every shape was `order=captured` after 7, 1 and 1
  statements and 1.2 s. The cost of a deferred shape (a search at every compile,
  no memo) is not timed here; `known-gaps.md` carries it.
- **No row-set sweep.** Rule 9's 200, 1K and 10K rows were not run: the cells are
  1-row point statements and two joins of 359 and 817 rows over fixed tables
  (`pc_users` 200, `pc_results` 2,872, `pc_loans` 10,000 rows scanned by the
  walk), the CB close's shapes. The overhead of a probe or a captured-table hit is a
  per-statement constant and the point cells say that; the per-row scaling was
  cut to fit the budget.
- **No server in a wait decomposition.** The measured unit is an overhead of
  about 0.5 µs on a 21 to 33 µs server path; no wait type is among its parts
  (a point read takes no lock wait, no fsync under `durability = group` and no
  conflict). The decomposition does not apply.
- **The attribution is coarse.** Two intermediate commits (X, Y), each CX step
  within the floor of its neighbours; DF-S1 and CX-S3b to CX-S7 are not separated.
  A bisect per stage was not run. The B - A delta is therefore not attributed
  to a single change, and the `sys.statement_stats` widening is named as part
  of it by the DF-S0 spike and not confirmed by an arm.
- **The control's spread is a property of the harness and not small at
  `cores = 2`.** A and A2 are two servers of one binary on one pinned CPU
  and differ in some runs by 2.8 % (run 7) and the model-join cell's CPU by up
  to 3 µs (run 4's P0b); a delta below that is not reported as a finding. The
  point-cell deltas at `cores = 1` (1.2 to 2.0 %) are above their controls in
  runs 1 and 3 (0.2 to 0.9 %) and no third `cores = 1` run of B against A was made.
- **Other sessions' processes were live** on the host throughout (loadavg 0.8
  to 3.0). No build or test ran during any block; the builds of X and Y ran between run 1 and run 3, not during a block.
- **Absolute numbers are this host's**; none is compared with the CB close.
  A server pinned to one CPU is a property of the method: the join cells'
  server CPU includes the sample from one thread; `cores = 2` servers were pinned
  to two.
- **Not measured:** the PostgreSQL floor for these shapes; a restart's
  `sys.plans` load; a point read under an enabled plan that does hit (the plan
  applies and the statement compiles twice, CX-S6); and the first-compile cost
  of a plan-governed statement.

## Reading for the milestone

- **CX did not slow the searched join shapes** (B and B' within 1.4 % of A on
  817-row and 359-row joins, inside the controls) and it did not change
  the result: B ran captured orders where A and B' ran memoised ones, `reordered=1` on
  both of the shapes A reorders, and every multiset was equal. The captured table is a hit instead of a memo hit.
- **A point statement costs 0.4 to 0.8 µs of CPU more at B than at A** (1.2 to
  2.0 % throughput, `cores = 1`, two runs, above the control in both). It is
  the same on `fixed` and `planned`, so it is not the plan table; DF-S1's
  bookkeeping is named a part of it by its spike and the intermediate arms are
  inside the floor one by one. If that is not acceptable, the next measurement
  is a per-stage bisect from `ee89842e` to `a32528a4` and a spike of `sys.statement_stats`
  at B; it was not run here.
- **An enabled one-relation plan puts a measurable but small probe on every
  one-relation `SELECT`** (about +0.1 to +0.25 µs of CPU, 0.5 to 1 % of a
  point read). `known-gaps.md`'s "Not measured; owed at CX's close" for that
  entry now has this value and a run that bounds it; the entry should say so
  and keep the cost as stated.
- **No claim is made for `cores = 2`**; its control spreads to 2.8 %.
